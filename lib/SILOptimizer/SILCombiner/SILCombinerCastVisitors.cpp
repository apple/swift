//===--- SILCombinerCastVisitors.cpp --------------------------------------===//
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

#define DEBUG_TYPE "sil-combine"

#include "SILCombiner.h"

#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/ValueTracking.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/DebugOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

using namespace swift;
using namespace swift::PatternMatch;

SILInstruction *
SILCombiner::visitRefToRawPointerInst(RefToRawPointerInst *rrpi) {
  if (auto *urci = dyn_cast<UncheckedRefCastInst>(rrpi->getOperand())) {
    // In this optimization, we try to move ref_to_raw_pointer up the def-use
    // graph. E.x.:
    //
    // ```
    // %0 = ...
    // %1 = unchecked_ref_cast %0
    // %2 = ref_to_raw_pointer %1
    // ```
    //
    // to:
    //
    // ```
    // %0 = ...
    // %2 = ref_to_raw_pointer %0
    // %1 = unchecked_ref_cast %0
    // ```
    //
    // If we find that the unchecked_ref_cast has no uses, we then eliminate
    // it.
    //
    // Naturally, this requires us to always hoist our new instruction (or
    // modified instruction) to before the unchecked_ref_cast.
    //
    // First we handle the case where we have a class type where we do not need
    // to insert a new instruction.
    if (urci->getOperand()->getType().isAnyClassReferenceType()) {
      rrpi->setOperand(urci->getOperand());
      rrpi->moveBefore(urci);
      return urci->use_empty() ? eraseInstFromFunction(*urci) : nullptr;
    }

    // Otherwise, we ened to use an unchecked_trivial_bit_cast insert it at
    // urci.
    //
    // (ref_to_raw_pointer (unchecked_ref_cast x))
    //    -> (unchecked_trivial_bit_cast x)
    auto *utbi = withBuilder(urci, [&](auto &b, auto l) {
      return b.createUncheckedTrivialBitCast(l, urci->getOperand(),
                                             rrpi->getType());
    });
    rrpi->replaceAllUsesWith(utbi);
    eraseInstFromFunction(*rrpi);
    return urci->use_empty() ? eraseInstFromFunction(*urci) : nullptr;
  }

  // (ref_to_raw_pointer (open_existential_ref (init_existential_ref x))) ->
  // (ref_to_raw_pointer x)
  //
  // In terms of ownership, we need to insert this at the init_existential to
  // ensure that x is live if we have an owned value.
  if (auto *oeri = dyn_cast<OpenExistentialRefInst>(rrpi->getOperand())) {
    if (auto *ieri = dyn_cast<InitExistentialRefInst>(oeri->getOperand())) {
      auto *utbi = withBuilder(ieri, [&](auto &b, auto l) {
        return b.createRefToRawPointer(l, ieri->getOperand(), rrpi->getType());
      });
      rrpi->replaceAllUsesWith(utbi);
      return eraseInstFromFunction(*rrpi);
    }
  }

  return nullptr;
}

namespace {

/// A folder object for sequences of forwarding instructions that forward owned
/// ownership. Is used to detect if we can delete the intermediate forwarding
/// instructions without ownership issues and then allows the user to either
/// delete all of the rest of the forwarding instructions and then replace front
/// with a new value or set front's operand to a new value.
class SingleBlockOwnedForwardingInstFolder {
  SmallVector<SingleValueInstruction *, 4> rest;
  SILCombiner &SC;
  SingleValueInstruction *front;

public:
  SingleBlockOwnedForwardingInstFolder(
      SILCombiner &SC, SingleValueInstruction *instructionToFold)
      : SC(SC), front(instructionToFold) {
    // If our initial instruction to fold isn't owned, set it to nullptr to
    // indicate invalid.
    if (SILValue(instructionToFold)->getOwnershipKind() != OwnershipKind::Owned)
      front = nullptr;
  }

  bool isValid() const { return bool(front); }

  bool add(SingleValueInstruction *next) {
    assert(isValid());
    if (SILValue(next)->getOwnershipKind() != OwnershipKind::Owned)
      return false;

    if (next->getSingleUse()) {
      rest.push_back(next);
      return true;
    }

    if (front->getParent() != next->getParent()) {
      return false;
    }

    // Otherwise, since the two values are in the same block and we want to
    // optimize only if our original value doesn't have any non-debug uses, we
    // know that our value can only have a single non-debug use, the consuming
    // user. So if we are not in that situation, bail.
    if (!hasOneNonDebugUse(next))
      return false;

    assert(rest.empty() || getSingleNonDebugUser(rest.back()) == next);
    rest.push_back(next);
    return true;
  }

  /// Delete all forwarding uses and then RAUW front with newValue.
  SingleValueInstruction *optimizeWithReplacement(SILValue newValue) && {
    // NOTE: Even though after running cleanup rest, front now has its
    // forwarding operand set to Undef, we haven't touched its result. So it is
    // safe to RAUW.
    cleanupRest();
    SC.replaceValueUsesWith(front, newValue);
    return nullptr;
  }

  /// Delete all forwarding uses and then set front's first operand to be \p
  /// newValue.
  SingleValueInstruction *optimizeWithSetValue(SILValue newValue) && {
    cleanupRest();
    assert(isa<SILUndef>(front->getOperand(0)));
    front->setOperand(0, newValue);
    SC.setUseValue(&front->getOperandRef(0), newValue);
    return nullptr;
  }

private:
  /// Processing from def->use by walking rest backwards, delete all of its
  /// debug uses and then set its single remaining use to be SILUndef.
  ///
  /// This means that after this runs front's forwarding operand is now
  /// SILUndef.
  void cleanupRest() & {
    // We process from def->use. This cleans up everything but the front value.
    while (!rest.empty()) {
      auto *inst = rest.pop_back_val();
      deleteAllDebugUses(inst, SC.getInstModCallbacks());
      auto *next = inst->getSingleUse();
      assert(next);
      assert(rest.empty() || bool(next->getUser() == rest.back()));
      next->set(SILUndef::get(next->get()->getType(), inst->getModule()));
      SC.eraseInstFromFunction(*inst);
    }
  }
};

} // namespace

SILInstruction *SILCombiner::visitUpcastInst(UpcastInst *uci) {
  auto operand = uci->getOperand();

  // %operandUpcast = upcast %0 : $X->Y
  // %upcastInst = upcast %operandUpcast : $Y->Z
  //
  // %operandUpcast = upcast %0 : $X->Y
  // %1 = upcast %0 : $X->Z
  //
  // If operandUpcast does not have any further uses, we delete it.
  if (auto *operandAsUpcast = dyn_cast<UpcastInst>(operand)) {
    if (operand->getOwnershipKind() != OwnershipKind::Owned) {
      uci->setOperand(operandAsUpcast->getOperand());
      return operandAsUpcast->use_empty()
                 ? eraseInstFromFunction(*operandAsUpcast)
                 : nullptr;
    }
    SingleBlockOwnedForwardingInstFolder folder(*this, uci);
    if (folder.add(operandAsUpcast)) {
      return std::move(folder).optimizeWithSetValue(
          operandAsUpcast->getOperand());
    }
  }

  return nullptr;
}

// Optimize Builtin.assumeAlignment -> pointer_to_address
//
// Case #1. Literal zero = natural alignment
//    %1 = integer_literal $Builtin.Int64, 0
//    %2 = builtin "assumeAlignment"
//         (%0 : $Builtin.RawPointer, %1 : $Builtin.Int64) : $Builtin.RawPointer
//    %3 = pointer_to_address %2 : $Builtin.RawPointer to [align=1] $*Int
//
//    Erases the `pointer_to_address` `[align=]` attribute:
//
// Case #2. Literal nonzero = forced alignment.
//
//    %1 = integer_literal $Builtin.Int64, 16
//    %2 = builtin "assumeAlignment"
//         (%0 : $Builtin.RawPointer, %1 : $Builtin.Int64) : $Builtin.RawPointer
//    %3 = pointer_to_address %2 : $Builtin.RawPointer to [align=1] $*Int
//
//    Promotes the `pointer_to_address` `[align=]` attribute to a higher value.
//
// Case #3. Folded dynamic alignment
//
//    %1 = builtin "alignof"<T>(%0 : $@thin T.Type) : $Builtin.Word
//    %2 = builtin "assumeAlignment"
//         (%0 : $Builtin.RawPointer, %1 : $Builtin.Int64) : $Builtin.RawPointer
//    %3 = pointer_to_address %2 : $Builtin.RawPointer to [align=1] $*T
//
//    Erases the `pointer_to_address` `[align=]` attribute.
SILInstruction *
SILCombiner::optimizeAlignment(PointerToAddressInst *ptrAdrInst) {
  if (!ptrAdrInst->alignment())
    return nullptr;

  llvm::Align oldAlign = ptrAdrInst->alignment().valueOrOne();

  // TODO: stripCasts(ptrAdrInst->getOperand()) can be used to find the Builtin,
  // but then the Builtin could not be trivially removed. Ideally,
  // Builtin.assume will be the immediate operand so it can be removed in the
  // common case.
  BuiltinInst *assumeAlign = dyn_cast<BuiltinInst>(ptrAdrInst->getOperand());
  if (!assumeAlign
      || assumeAlign->getBuiltinKind() != BuiltinValueKind::AssumeAlignment) {
    return nullptr;
  }
  SILValue ptrSrc = assumeAlign->getArguments()[0];
  SILValue alignOper = assumeAlign->getArguments()[1];

  if (auto *integerInst = dyn_cast<IntegerLiteralInst>(alignOper)) {
    llvm::MaybeAlign newAlign(integerInst->getValue().getLimitedValue());
    if (newAlign && newAlign.valueOrOne() <= oldAlign)
      return nullptr;

    // Case #1: the pointer is assumed naturally aligned
    //
    // Or Case #2: the pointer is assumed to have non-zero alignment greater
    // than it current alignment.
    //
    // In either case, rewrite the address alignment with the assumed alignment,
    // and bypass the Builtin.assumeAlign.
    return Builder.createPointerToAddress(
        ptrAdrInst->getLoc(), ptrSrc, ptrAdrInst->getType(),
        ptrAdrInst->isStrict(), ptrAdrInst->isInvariant(), newAlign);
  }
  // Handle possible 32-bit sign-extension.
  SILValue extendedAlignment;
  if (match(alignOper,
            m_ApplyInst(BuiltinValueKind::SExtOrBitCast,
                        m_ApplyInst(BuiltinValueKind::TruncOrBitCast,
                                    m_SILValue(extendedAlignment))))) {
    alignOper = extendedAlignment;
  }
  if (match(alignOper,
            m_ApplyInst(BuiltinValueKind::Alignof))) {
    CanType formalType = cast<BuiltinInst>(alignOper)->getSubstitutions()
      .getReplacementTypes()[0]->getReducedType(
          ptrAdrInst->getFunction()->getGenericSignature());

    SILType instanceType = ptrAdrInst->getFunction()->getLoweredType(
      Lowering::AbstractionPattern::getOpaque(), formalType);

    if (instanceType.getAddressType() != ptrAdrInst->getType())
      return nullptr;

    // Case #3: the alignOf type matches the address type. Convert to a
    // naturally aligned pointer by erasing alignment and bypassing the
    // Builtin.assumeAlign.
    return Builder.createPointerToAddress(
        ptrAdrInst->getLoc(), ptrSrc, ptrAdrInst->getType(),
        ptrAdrInst->isStrict(), ptrAdrInst->isInvariant());
  }
  return nullptr;
}

SILInstruction *
SILCombiner::
visitPointerToAddressInst(PointerToAddressInst *PTAI) {
  auto *F = PTAI->getFunction();

  Builder.setCurrentDebugScope(PTAI->getDebugScope());

  // If we reach this point, we know that the types must be different since
  // otherwise simplifyInstruction would have handled the identity case. This is
  // always legal to do since address-to-pointer pointer-to-address implies
  // layout compatibility.
  //
  // (pointer-to-address strict (address-to-pointer %x))
  // -> (unchecked_addr_cast %x)
  if (PTAI->isStrict()) {
    // We can not perform this optimization with ownership until we are able to
    // handle issues around interior pointers and expanding borrow scopes.
    if (auto *ATPI = dyn_cast<AddressToPointerInst>(PTAI->getOperand())) {
      if (!hasOwnership()) {
        return Builder.createUncheckedAddrCast(PTAI->getLoc(),
                                               ATPI->getOperand(),
                                               PTAI->getType());
      }

      OwnershipRAUWHelper helper(ownershipFixupContext, PTAI,
                                 ATPI->getOperand());
      if (helper) {
        auto replacement = helper.prepareReplacement();
        auto *newInst = Builder.createUncheckedAddrCast(
            PTAI->getLoc(), replacement, PTAI->getType());
        helper.perform(newInst);
        return nullptr;
      }
    }
  }

  // The rest of these canonicalizations optimize the code around
  // pointer_to_address by leave in a pointer_to_address meaning that we do not
  // need to worry about moving addresses out of interior pointer scopes.

  // Turn this also into an index_addr. We generate this pattern after switching
  // the Word type to an explicit Int32 or Int64 in the stdlib.
  //
  // %101 = builtin "strideof"<Int>(%84 : $@thick Int.Type) :
  //         $Builtin.Word
  // %102 = builtin "zextOrBitCast_Word_Int64"(%101 : $Builtin.Word) :
  //         $Builtin.Int64
  // %111 = builtin "smul_with_overflow_Int64"(%108 : $Builtin.Int64,
  //                               %102 : $Builtin.Int64, %20 : $Builtin.Int1) :
  //         $(Builtin.Int64, Builtin.Int1)
  // %112 = tuple_extract %111 : $(Builtin.Int64, Builtin.Int1), 0
  // %113 = builtin "truncOrBitCast_Int64_Word"(%112 : $Builtin.Int64) :
  //         $Builtin.Word
  // %114 = index_raw_pointer %100 : $Builtin.RawPointer, %113 : $Builtin.Word
  // %115 = pointer_to_address %114 : $Builtin.RawPointer to [strict] $*Int
  //
  // This is safe for ownership since our final SIL still has a
  // pointer_to_address meaning that we do not need to worry about interior
  // pointers.
  SILValue Distance;
  SILValue TruncOrBitCast;
  MetatypeInst *Metatype;
  IndexRawPointerInst *IndexRawPtr;
  BuiltinInst *StrideMul;
  if (match(
          PTAI->getOperand(),
          m_IndexRawPointerInst(IndexRawPtr))) {
    SILValue Ptr = IndexRawPtr->getOperand(0);
    SILValue TruncOrBitCast = IndexRawPtr->getOperand(1);
    if (match(TruncOrBitCast, m_ApplyInst(BuiltinValueKind::TruncOrBitCast,
                                          m_TupleExtractOperation(
                                              m_BuiltinInst(StrideMul), 0)))) {
      if (match(StrideMul,
                m_ApplyInst(
                    BuiltinValueKind::SMulOver, m_SILValue(Distance),
                    m_ApplyInst(BuiltinValueKind::ZExtOrBitCast,
                                m_ApplyInst(BuiltinValueKind::Strideof,
                                            m_MetatypeInst(Metatype))))) ||
          match(StrideMul,
                m_ApplyInst(
                    BuiltinValueKind::SMulOver,
                    m_ApplyInst(BuiltinValueKind::ZExtOrBitCast,
                                m_ApplyInst(BuiltinValueKind::Strideof,
                                            m_MetatypeInst(Metatype))),
                    m_SILValue(Distance)))) {

        SILType InstanceType =
            F->getLoweredType(Metatype->getType()
                .castTo<MetatypeType>().getInstanceType());

        auto *Trunc = cast<BuiltinInst>(TruncOrBitCast);

        // Make sure that the type of the metatype matches the type that we are
        // casting to so we stride by the correct amount.
        if (InstanceType.getAddressType() != PTAI->getType()) {
          return nullptr;
        }

        auto *NewPTAI = Builder.createPointerToAddress(PTAI->getLoc(), Ptr,
                                                       PTAI->getType(),
                                                       PTAI->isStrict(),
                                                       PTAI->isInvariant());
        auto DistanceAsWord = Builder.createBuiltin(
            PTAI->getLoc(), Trunc->getName(), Trunc->getType(), {}, Distance);

        return Builder.createIndexAddr(PTAI->getLoc(), NewPTAI, DistanceAsWord,
                                       /*needsStackProtection=*/ false);
      }
    }
  }

  // Turn:
  //
  //   %stride = Builtin.strideof(T) * %distance
  //   %ptr' = index_raw_pointer %ptr, %stride
  //   %result = pointer_to_address %ptr, [strict] $T'
  //
  // To:
  //
  //   %addr = pointer_to_address %ptr, [strict] $T
  //   %result = index_addr %addr, %distance
  //
  // This is safe for ownership since our final SIL still has a
  // pointer_to_address meaning that we do not need to worry about interior
  // pointers.
  BuiltinInst *Bytes = nullptr;
  if (match(PTAI->getOperand(),
            m_IndexRawPointerInst(
                m_ValueBase(),
                m_TupleExtractOperation(m_BuiltinInst(Bytes), 0)))) {
    assert(Bytes != nullptr &&
           "Bytes should have been assigned a non-null value");
    if (match(Bytes, m_ApplyInst(BuiltinValueKind::SMulOver, m_ValueBase(),
                                 m_ApplyInst(BuiltinValueKind::Strideof,
                                             m_MetatypeInst(Metatype)),
                                 m_ValueBase()))) {

      SILType InstanceType =
          F->getLoweredType(Metatype->getType()
              .castTo<MetatypeType>().getInstanceType());

      // Make sure that the type of the metatype matches the type that we are
      // casting to so we stride by the correct amount.
      if (InstanceType.getAddressType() != PTAI->getType())
        return nullptr;

      auto IRPI = cast<IndexRawPointerInst>(PTAI->getOperand());
      SILValue Ptr = IRPI->getOperand(0);
      SILValue Distance = Bytes->getArguments()[0];
      auto *NewPTAI =
        Builder.createPointerToAddress(PTAI->getLoc(), Ptr, PTAI->getType(),
                                       PTAI->isStrict(), PTAI->isInvariant());
      return Builder.createIndexAddr(PTAI->getLoc(), NewPTAI, Distance,
                                     /*needsStackProtection=*/ false);
    }
  }

  return optimizeAlignment(PTAI);
}

SILInstruction *
SILCombiner::visitUncheckedAddrCastInst(UncheckedAddrCastInst *UADCI) {
  // These are always safe to perform due to interior pointer ownership
  // requirements being transitive along addresses.

  Builder.setCurrentDebugScope(UADCI->getDebugScope());

  // (unchecked_addr_cast (unchecked_addr_cast x X->Y) Y->Z)
  //   ->
  // (unchecked_addr_cast x X->Z)
  if (auto *OtherUADCI = dyn_cast<UncheckedAddrCastInst>(UADCI->getOperand()))
    return Builder.createUncheckedAddrCast(UADCI->getLoc(),
                                           OtherUADCI->getOperand(),
                                           UADCI->getType());
  return nullptr;
}

SILInstruction *
SILCombiner::visitUncheckedRefCastInst(UncheckedRefCastInst *urci) {
  // %0 = unchecked_ref_cast %x : $X->Y
  // %1 = unchecked_ref_cast %0 : $Y->Z
  //
  // ->
  //
  // %0 = unchecked_ref_cast %x : $X->Y
  // %1 = unchecked_ref_cast %x : $X->Z
  //
  // NOTE: For owned values, we only perform this optimization if we can
  // guarantee that we can eliminate the initial unchecked_ref_cast.
  if (auto *otherURCI = dyn_cast<UncheckedRefCastInst>(urci->getOperand())) {
    SILValue otherURCIOp = otherURCI->getOperand();
    if (otherURCIOp->getOwnershipKind() != OwnershipKind::Owned) {
      return Builder.createUncheckedRefCast(urci->getLoc(), otherURCIOp,
                                            urci->getType());
    }
    SingleBlockOwnedForwardingInstFolder folder(*this, urci);
    if (folder.add(otherURCI)) {
      auto *newValue = Builder.createUncheckedRefCast(
          urci->getLoc(), otherURCIOp, urci->getType());
      return std::move(folder).optimizeWithReplacement(newValue);
    }
  }

  // %0 = upcast %x : $X->Y
  // %1 = unchecked_ref_cast %0 : $Y->Z
  //
  // ->
  //
  // %0 = upcast %x : $X->Y
  // %1 = unchecked_ref_cast %x : $X->Z
  //
  // NOTE: For owned values, we only perform this optimization if we can
  // guarantee that we can eliminate the upcast.
  if (auto *ui = dyn_cast<UpcastInst>(urci->getOperand())) {
    SILValue uiOp = ui->getOperand();

    if (uiOp->getOwnershipKind() != OwnershipKind::Owned) {
      return Builder.createUncheckedRefCast(urci->getLoc(), uiOp,
                                            urci->getType());
    }

    SingleBlockOwnedForwardingInstFolder folder(*this, urci);
    if (folder.add(ui)) {
      auto *newValue =
          Builder.createUncheckedRefCast(urci->getLoc(), uiOp, urci->getType());
      return std::move(folder).optimizeWithReplacement(newValue);
    }
  }

  // This is an exact transform where we are replacing urci with an upcast on
  // the same value. So from an ownership perspective because both instructions
  // are forwarding and we are eliminating urci, we are safe.
  if (urci->getType() != urci->getOperand()->getType() &&
      urci->getType().isExactSuperclassOf(urci->getOperand()->getType()))
    return Builder.createUpcast(urci->getLoc(), urci->getOperand(),
                                urci->getType());

  // %0 = init_existential_ref %x : $X -> Existential
  // %1 = open_existential_ref %0 : $Existential -> @opened() Existential
  // %2 = unchecked_ref_cast %1
  //
  // ->
  //
  // %0 = init_existential_ref %x : $X -> Existential
  // %1 = open_existential_ref %0 : $Existential -> @opened() Existential
  // %2 = unchecked_ref_cast %x
  //
  // NOTE: When we have an owned value, we only perform this optimization if we
  // can remove both the open_existential_ref and the init_existential_ref.
  if (auto *oer = dyn_cast<OpenExistentialRefInst>(urci->getOperand())) {
    if (auto *ier = dyn_cast<InitExistentialRefInst>(oer->getOperand())) {
      if (ier->getForwardingOwnershipKind() != OwnershipKind::Owned) {
        return Builder.createUncheckedRefCast(urci->getLoc(), ier->getOperand(),
                                              urci->getType());
      }

      SingleBlockOwnedForwardingInstFolder folder(*this, urci);
      if (folder.add(oer) && folder.add(ier)) {
        auto *newValue = Builder.createUncheckedRefCast(
            urci->getLoc(), ier->getOperand(), urci->getType());
        return std::move(folder).optimizeWithReplacement(newValue);
      }
    }
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitEndCOWMutationInst(EndCOWMutationInst *ECM) {

  // Remove a cast if it's only used by an end_cow_mutation.
  //
  // (end_cow_mutation (upcast X)) -> (end_cow_mutation X)
  // (end_cow_mutation (unchecked_ref_cast X)) -> (end_cow_mutation X)
  SILValue op = ECM->getOperand();
  if (!isa<UncheckedRefCastInst>(op) && !isa<UpcastInst>(op))
    return nullptr;
  if (!op->hasOneUse())
    return nullptr;

  SingleValueInstruction *refCast = cast<SingleValueInstruction>(op);
  auto *newECM = Builder.createEndCOWMutation(ECM->getLoc(),
                                              refCast->getOperand(0));
  ECM->replaceAllUsesWith(refCast);
  refCast->setOperand(0, newECM);
  refCast->moveAfter(newECM);
  return eraseInstFromFunction(*ECM);
}

SILInstruction *
SILCombiner::visitBridgeObjectToRefInst(BridgeObjectToRefInst *bori) {
  // Fold noop casts through Builtin.BridgeObject.
  //
  // (bridge_object_to_ref (unchecked-ref-cast x BridgeObject) y)
  //  -> (unchecked-ref-cast x y)
  if (auto *urc = dyn_cast<UncheckedRefCastInst>(bori->getOperand())) {
    if (SILValue(urc)->getOwnershipKind() != OwnershipKind::Owned) {
      return Builder.createUncheckedRefCast(
          bori->getLoc(), urc->getOperand(), bori->getType());
    }
    SingleBlockOwnedForwardingInstFolder folder(*this, bori);
    if (folder.add(urc)) {
      auto *newValue = Builder.createUncheckedRefCast(
          bori->getLoc(), urc->getOperand(), bori->getType());
      return std::move(folder).optimizeWithReplacement(newValue);
    }
  }

  return nullptr;
}

SILInstruction *
SILCombiner::visitUncheckedRefCastAddrInst(UncheckedRefCastAddrInst *urci) {
  // Promote unchecked_ref_cast_addr in between two loadable values to
  // unchecked_ref_cast upon objects.
  //
  // NOTE: unchecked_ref_cast_addr is a taking operation, so we simulate that
  // with objects.
  SILType srcTy = urci->getSrc()->getType();
  if (!srcTy.isLoadable(*urci->getFunction()))
    return nullptr;

  SILType destTy = urci->getDest()->getType();
  if (!destTy.isLoadable(*urci->getFunction()))
    return nullptr;

  // After promoting unchecked_ref_cast_addr to unchecked_ref_cast, the SIL
  // verifier will assert that the loadable source and dest type of reference
  // castable. If the static types are invalid, simply avoid promotion, that way
  // the runtime will then report a failure if this cast is ever executed.
  if (!SILType::canRefCast(srcTy.getObjectType(), destTy.getObjectType(),
                           urci->getModule()))
    return nullptr;
 
  SILLocation loc = urci->getLoc();
  Builder.setCurrentDebugScope(urci->getDebugScope());
  SILValue load = Builder.emitLoadValueOperation(loc, urci->getSrc(),
                                                 LoadOwnershipQualifier::Take);

  assert(SILType::canRefCast(load->getType(), destTy.getObjectType(),
                             Builder.getModule()) &&
         "SILBuilder cannot handle reference-castable types");
  auto *cast = Builder.createUncheckedRefCast(loc, load,
                                              destTy.getObjectType());
  Builder.emitStoreValueOperation(loc, cast, urci->getDest(),
                                  StoreOwnershipQualifier::Init);

  return eraseInstFromFunction(*urci);
}

template <class CastInst>
static bool canBeUsedAsCastDestination(SILValue value, CastInst *castInst,
                                       DominanceAnalysis *DA) {
  return value &&
         value->getType() == castInst->getTargetLoweredType().getObjectType() &&
         DA->get(castInst->getFunction())->properlyDominates(value, castInst);
}

SILInstruction *SILCombiner::visitUnconditionalCheckedCastAddrInst(
    UnconditionalCheckedCastAddrInst *uccai) {

  // Optimize the unconditional_checked_cast_addr in the following non-ossa/ossa
  // pattern:
  //
  // Non-OSSA Pattern
  //
  //   %value = ...
  //   ...
  //   %box = alloc_existential_box $Error, $ConcreteError
  //   %a = project_existential_box $ConcreteError in %b : $Error
  //   store %value to %a : $*ConcreteError
  //   %err = alloc_stack $Error
  //   store %box to %err : $*Error
  //   %dest = alloc_stack $ConcreteError
  //   unconditional_checked_cast_addr Error in %err : $*Error to
  //                                ConcreteError in %dest : $*ConcreteError
  //
  // to:
  //
  //   retain_value %value : $ConcreteError
  //   ...
  //   %box = alloc_existential_box $Error, $ConcreteError
  //   %a = project_existential_box $ConcreteError in %b : $Error
  //   store %value to %a : $*ConcreteError
  //   %err = alloc_stack $Error
  //   store %box to %err : $*Error
  //   destroy_addr %err : $*Error
  //   store %value to %dest $*ConcreteError
  //
  // OSSA Pattern:
  //
  //   %value = ...
  //   ...
  //   %box = alloc_existential_box $Error, $ConcreteError
  //   %a = project_existential_box $ConcreteError in %b : $Error
  //   store %value to [init] %a : $*ConcreteError
  //   %err = alloc_stack $Error
  //   store %box to [init] %err : $*Error
  //   %dest = alloc_stack $ConcreteError
  //   unconditional_checked_cast_addr Error in %err : $*Error to
  //                                ConcreteError in %dest : $*ConcreteError
  //
  // to:
  //
  //   %value_copy = copy_value %value
  //   ...
  //   %box = alloc_existential_box $Error, $ConcreteError
  //   %a = project_existential_box $ConcreteError in %b : $Error
  //   store %value to [init] %a : $*ConcreteError
  //   %err = alloc_stack $Error
  //   store %box to [init] %err : $*Error
  //   destroy_addr %err : $*Error
  //   store %value to %dest $*ConcreteError
  //
  // In both cases, this lets the alloc_existential_box become dead and it can
  // be removed in other subsequent optimizations.
  SILValue val = getConcreteValueOfExistentialBoxAddr(uccai->getSrc(), uccai);
  while (auto *cvi = dyn_cast_or_null<CopyValueInst>(val))
    val = cvi->getOperand();
  if (canBeUsedAsCastDestination(val, uccai, DA)) {
    // We need to copy the value at its insertion point.
    {
      auto *nextInsertPt = val->getNextInstruction();
      if (!nextInsertPt)
        return nullptr;
      // If our value is defined by an instruction (not an argument), we want to
      // insert the copy after that. Otherwise, we have an argument and we want
      // to insert the copy right at the beginning of the block.
      SILBuilderWithScope builder(nextInsertPt, Builder);
      // We use an autogenerated location to ensure that if next is a
      // terminator, we do not trip an assertion around mismatched debug info.
      //
      // FIXME: We should find a better way of solving this than losing location
      // info!
      auto loc = RegularLocation::getAutoGeneratedLocation();
      val = builder.emitCopyValueOperation(loc, val);
    }

    // Then we insert the destroy addr/store at the cast location.
    SILBuilderWithScope builder(uccai, Builder);
    SILLocation loc = uccai->getLoc();
    builder.createDestroyAddr(loc, uccai->getSrc());
    builder.emitStoreValueOperation(loc, val, uccai->getDest(),
                                    StoreOwnershipQualifier::Init);
    return eraseInstFromFunction(*uccai);
  }

  // Perform the purly type-based cast optimization.
  if (CastOpt.optimizeUnconditionalCheckedCastAddrInst(uccai))
    MadeChange = true;

  return nullptr;
}

SILInstruction *
SILCombiner::
visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI) {
  CastOpt.optimizeUnconditionalCheckedCastInst(UCCI);
  if (UCCI->isDeleted()) {
    MadeChange = true;
    return nullptr;
  }
  // FIXME: rename from RemoveCondFails to RemoveRuntimeAsserts.
  if (RemoveCondFails) {
    auto LoweredTargetType = UCCI->getType();
    auto Loc = UCCI->getLoc();
    auto Op = UCCI->getOperand();
    if (LoweredTargetType.isAddress()) {
      // unconditional_checked_cast -> unchecked_addr_cast
      return Builder.createUncheckedAddrCast(Loc, Op, LoweredTargetType);
    } else if (LoweredTargetType.isHeapObjectReferenceType()) {
      if (!(Op->getType().isHeapObjectReferenceType() ||
            Op->getType().isClassExistentialType())) {
        return nullptr;
      }
      // unconditional_checked_cast -> unchecked_ref_cast
      return Builder.createUncheckedRefCast(Loc, Op, LoweredTargetType);
    }
  }

  return nullptr;
}

SILInstruction *
SILCombiner::visitRawPointerToRefInst(RawPointerToRefInst *rawToRef) {
  // (raw_pointer_to_ref (ref_to_raw_pointer x X->Y) Y->Z)
  //   ->
  // (unchecked_ref_cast x X->Z)
  if (auto *refToRaw = dyn_cast<RefToRawPointerInst>(rawToRef->getOperand())) {
    if (!hasOwnership()) {
      return Builder.createUncheckedRefCast(
          rawToRef->getLoc(), refToRaw->getOperand(), rawToRef->getType());
    }

    // raw_pointer_to_ref produces an unowned value. So we need to handle it
    // especially with ownership.
    {
      SILValue originalRef = refToRaw->getOperand();
      OwnershipRAUWHelper helper(ownershipFixupContext, rawToRef, originalRef);
      if (helper) {
        // Since we are using std::next, we use getAutogeneratedLocation to
        // avoid any issues if our next insertion point is a terminator.
        auto loc = RegularLocation::getAutoGeneratedLocation();
        auto replacement = helper.prepareReplacement();
        auto *newInst = Builder.createUncheckedRefCast(
            loc, replacement, rawToRef->getType());
        // If we have an operand with ownership, we need to change our
        // unchecked_ref_cast to produce an unowned value. This is because
        // otherwise, our unchecked_ref_cast will consume the underlying owned
        // value, changing a BitwiseEscape to a LifetimeEnding use?! In
        // contrast, for guaranteed, we are replacing a BitwiseEscape use
        // (ref_to_rawpointer) with a ForwardedBorrowingUse (unchecked_ref_cast)
        // which is safe.
        if (newInst->getForwardingOwnershipKind() == OwnershipKind::Owned) {
          newInst->setForwardingOwnershipKind(OwnershipKind::Unowned);
        }
        helper.perform(newInst);
        return nullptr;
      }
    }
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitUncheckedTrivialBitCastInst(
    UncheckedTrivialBitCastInst *utbci) {
  // (unchecked_trivial_bit_cast Y->Z
  //                                 (unchecked_trivial_bit_cast X->Y x))
  //   ->
  // (unchecked_trivial_bit_cast X->Z x)
  SILValue operand = utbci->getOperand();
  if (auto *otherUTBCI = dyn_cast<UncheckedTrivialBitCastInst>(operand)) {
    return Builder.createUncheckedTrivialBitCast(
        utbci->getLoc(), otherUTBCI->getOperand(), utbci->getType());
  }

  // %y = unchecked_ref_cast %x X->Y
  // ...
  // %z = unchecked_trivial_bit_cast %y Y->Z
  //
  //   ->
  //
  // %z = unchecked_trivial_bit_cast %x X->Z
  // %y = unchecked_ref_cast %x X->Y
  // ...
  if (auto *urbci = dyn_cast<UncheckedRefCastInst>(operand)) {
    // We just move the unchecked_trivial_bit_cast to before the
    // unchecked_ref_cast and then make its operand the unchecked_ref_cast
    // operand. Then we return the cast so we reprocess given that we changed
    // its operands.
    utbci->moveBefore(urbci);
    utbci->setDebugLocation(urbci->getDebugLocation());
    utbci->setOperand(urbci->getOperand());
    return utbci;
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *UBCI) {
  // (unchecked_bitwise_cast Y->Z (unchecked_bitwise_cast X->Y x))
  // OR (unchecked_trivial_cast Y->Z (unchecked_bitwise_cast X->Y x))
  //   ->
  // (unchecked_bitwise_cast X->Z x)
  SILValue Oper;
  if (match(UBCI->getOperand(),
            m_CombineOr(m_UncheckedBitwiseCastInst(m_SILValue(Oper)),
                        m_UncheckedTrivialBitCastInst(m_SILValue(Oper))))) {
    if (!Builder.hasOwnership()) {
      return Builder.createUncheckedBitwiseCast(UBCI->getLoc(), Oper,
                                                UBCI->getType());
    }

    OwnershipRAUWHelper helper(ownershipFixupContext, UBCI, Oper);
    if (helper) {
      auto replacement = helper.prepareReplacement();
      auto *transformedOper = Builder.createUncheckedBitwiseCast(
          UBCI->getLoc(), replacement, UBCI->getType());
      helper.perform(transformedOper);
      return nullptr;
    }
  }

  if (UBCI->getType().isTrivial(*UBCI->getFunction())) {
    // If our result is trivial, we can always just RAUW.
    return Builder.createUncheckedTrivialBitCast(
        UBCI->getLoc(), UBCI->getOperand(), UBCI->getType());
  }

  if (!SILType::canRefCast(UBCI->getOperand()->getType(), UBCI->getType(),
                           Builder.getModule()))
    return nullptr;

  // Normally, OwnershipRAUWHelper needs to be called to handle ownership of
  // UBCI->getOperand(). However, we know that UBCI->getOperand() is already
  // available at the point of the cast, and by forcing the cast to be Unowned,
  // we ensure that no ownership adjustment is needed. So we can skip
  // prepareReplacement completely and just drop in the replacement. That avoids
  // an extra copy in the case that UBCI->getOperand() is Owned.
  auto *refCast = Builder.createUncheckedRefCast(
      UBCI->getLoc(), UBCI->getOperand(), UBCI->getType());
  if (Builder.hasOwnership()) {
    // A bitwise cast is always unowned, so we can safely force the reference
    // cast to forward as unowned and no ownership adjustment is needed.
    assert(UBCI->getOwnershipKind() == OwnershipKind::Unowned);
    refCast->setForwardingOwnershipKind(OwnershipKind::Unowned);
  }
  return refCast;
}

SILInstruction *
SILCombiner::visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *TTOCMI) {
  if (auto *OCTTMI = dyn_cast<ObjCToThickMetatypeInst>(TTOCMI->getOperand())) {
    TTOCMI->replaceAllUsesWith(OCTTMI->getOperand());
    return eraseInstFromFunction(*TTOCMI);
  }

  // Perform the following transformations:
  // (thick_to_objc_metatype (metatype @thick)) ->
  // (metatype @objc_metatype)
  //
  // (thick_to_objc_metatype (value_metatype @thick)) ->
  // (value_metatype @objc_metatype)
  //
  // (thick_to_objc_metatype (existential_metatype @thick)) ->
  // (existential_metatype @objc_metatype)
  if (CastOpt.optimizeMetatypeConversion(ConversionOperation(TTOCMI),
                                         MetatypeRepresentation::Thick))
    MadeChange = true;

  return nullptr;
}

SILInstruction *
SILCombiner::visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *OCTTMI) {
  if (auto *TTOCMI = dyn_cast<ThickToObjCMetatypeInst>(OCTTMI->getOperand())) {
    OCTTMI->replaceAllUsesWith(TTOCMI->getOperand());
    return eraseInstFromFunction(*OCTTMI);
  }

  // Perform the following transformations:
  // (objc_to_thick_metatype (metatype @objc_metatype)) ->
  // (metatype @thick)
  //
  // (objc_to_thick_metatype (value_metatype @objc_metatype)) ->
  // (value_metatype @thick)
  //
  // (objc_to_thick_metatype (existential_metatype @objc_metatype)) ->
  // (existential_metatype @thick)
  if (CastOpt.optimizeMetatypeConversion(ConversionOperation(OCTTMI),
                                         MetatypeRepresentation::ObjC))
    MadeChange = true;

  return nullptr;
}

SILInstruction *
SILCombiner::visitCheckedCastBranchInst(CheckedCastBranchInst *CBI) {
  if (CastOpt.optimizeCheckedCastBranchInst(CBI))
    MadeChange = true;

  return nullptr;
}

SILInstruction *
SILCombiner::
visitCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *CCABI) {
  // Optimize the checked_cast_addr_br in this pattern:
  //
  //   %box = alloc_existential_box $Error, $ConcreteError
  //   %a = project_existential_box $ConcreteError in %b : $Error
  //   store %value to %a : $*ConcreteError
  //   %err = alloc_stack $Error
  //   store %box to %err : $*Error
  //   %dest = alloc_stack $ConcreteError
  //   checked_cast_addr_br <consumption-kind> Error in %err : $*Error to
  //        ConcreteError in %dest : $*ConcreteError, success_bb, failing_bb
  //
  // to:
  //   ...
  //   retain_value %value : $ConcreteError
  //   destroy_addr %err : $*Error           // if consumption-kind is take
  //   store %value to %dest $*ConcreteError
  //   br success_bb
  //
  // This lets the alloc_existential_box become dead and it can be removed in
  // following optimizations.
  //
  // TODO: Also handle the WillFail case.
  SILValue val = getConcreteValueOfExistentialBoxAddr(CCABI->getSrc(), CCABI);
  while (auto *cvi = dyn_cast_or_null<CopyValueInst>(val))
    val = cvi->getOperand();
  if (canBeUsedAsCastDestination(val, CCABI, DA)) {
    // We need to insert the copy after the defining instruction of val or at
    // the top of the block if val is an argument.
    {
      auto *nextInsertPt = val->getNextInstruction();
      if (!nextInsertPt)
        return nullptr;
      SILBuilderWithScope builder(nextInsertPt, Builder);
      auto loc = RegularLocation::getAutoGeneratedLocation();
      val = builder.emitCopyValueOperation(loc, val);
    }

    SILBuilderWithScope builder(CCABI, Builder);
    SILLocation loc = CCABI->getLoc();
    switch (CCABI->getConsumptionKind()) {
      case CastConsumptionKind::TakeAlways:
      case CastConsumptionKind::TakeOnSuccess:
        builder.createDestroyAddr(loc, CCABI->getSrc());
        break;
      case CastConsumptionKind::CopyOnSuccess:
        break;
      case CastConsumptionKind::BorrowAlways:
        llvm_unreachable("BorrowAlways is not supported on addresses");
    }
    builder.emitStoreValueOperation(loc, val, CCABI->getDest(),
                                    StoreOwnershipQualifier::Init);

    // Replace the cast with a constant conditional branch.
    // Don't just create an unconditional branch to not change the CFG in
    // SILCombine. SimplifyCFG will clean that up.
    //
    // Another possibility would be to run this optimization in SimplifyCFG.
    // But this has other problems, like it's more difficult to reason about a
    // consistent dominator tree in SimplifyCFG.
    SILType boolTy = SILType::getBuiltinIntegerType(1, builder.getASTContext());
    auto *trueVal = builder.createIntegerLiteral(loc, boolTy, 1);
    builder.createCondBranch(loc, trueVal, CCABI->getSuccessBB(),
                             CCABI->getFailureBB());
    return eraseInstFromFunction(*CCABI);
  }

  // Perform the purly type-based cast optimization.
  if (CastOpt.optimizeCheckedCastAddrBranchInst(CCABI))
    MadeChange = true;

  return nullptr;
}

SILInstruction *SILCombiner::visitConvertEscapeToNoEscapeInst(
    ConvertEscapeToNoEscapeInst *Cvt) {
  // Rewrite conversion of `convert_function` of `thin_to_thick_function` as
  // conversion of `thin_to_thick_function` of `convert_function`.
  //
  // (convert_escape_to_noescape (convert_function (thin_to_thick_function x)))
  // =>
  // (convert_escape_to_noescape (thin_to_thick_function (convert_function x)))
  //
  // This unblocks the `thin_to_thick_function` peephole optimization below.
  if (auto *CFI = dyn_cast<ConvertFunctionInst>(Cvt->getOperand())) {
    if (CFI->getSingleUse()) {
      if (auto *TTTFI = dyn_cast<ThinToThickFunctionInst>(CFI->getOperand())) {
        if (TTTFI->getSingleUse()) {
          auto convertedThickType = CFI->getType().castTo<SILFunctionType>();
          auto convertedThinType = convertedThickType->getWithRepresentation(
            SILFunctionTypeRepresentation::Thin);
          auto *newCFI = Builder.createConvertFunction(
              CFI->getLoc(), TTTFI->getOperand(),
              SILType::getPrimitiveObjectType(convertedThinType),
              CFI->withoutActuallyEscaping());
          auto *newTTTFI = Builder.createThinToThickFunction(
            TTTFI->getLoc(), newCFI, CFI->getType());
          replaceInstUsesWith(*CFI, newTTTFI);
        }
      }
    }
  }

  // Rewrite conversion of `thin_to_thick_function` as `thin_to_thick_function`
  // with a noescape function type.
  //
  // (convert_escape_to_noescape (thin_to_thick_function x)) =>
  // (thin_to_thick_function [noescape] x)
  if (auto *OrigThinToThick =
          dyn_cast<ThinToThickFunctionInst>(Cvt->getOperand())) {
    auto origFunType = OrigThinToThick->getType().getAs<SILFunctionType>();
    auto NewTy = origFunType->getWithExtInfo(origFunType->getExtInfo().withNoEscape(true));

    return Builder.createThinToThickFunction(
      OrigThinToThick->getLoc(), OrigThinToThick->getOperand(),
      SILType::getPrimitiveObjectType(NewTy));
  }

  // Push conversion instructions inside `differentiable_function`. This
  // unblocks more optimizations.
  //
  // Before:
  // %x = differentiable_function(%orig, %jvp, %vjp)
  // %y = convert_escape_to_noescape %x
  //
  // After:
  // %orig' = convert_escape_to_noescape %orig
  // %jvp' = convert_escape_to_noescape %jvp
  // %vjp' = convert_escape_to_noescape %vjp
  // %y = differentiable_function(%orig', %jvp', %vjp')
  if (auto *DFI = dyn_cast<DifferentiableFunctionInst>(Cvt->getOperand())) {
    auto createConvertEscapeToNoEscape = [&](NormalDifferentiableFunctionTypeComponent extractee) {
      if (!DFI->hasExtractee(extractee))
        return SILValue();
        
      auto operand = DFI->getExtractee(extractee);
      auto fnType = operand->getType().castTo<SILFunctionType>();
      auto noEscapeFnType =
        fnType->getWithExtInfo(fnType->getExtInfo().withNoEscape());
      auto noEscapeType = SILType::getPrimitiveObjectType(noEscapeFnType);
      return Builder.createConvertEscapeToNoEscape(
        operand.getLoc(), operand, noEscapeType, Cvt->isLifetimeGuaranteed())->getResult(0);
    };
    
    SILValue originalNoEscape =
      createConvertEscapeToNoEscape(NormalDifferentiableFunctionTypeComponent::Original);
    SILValue convertedJVP = createConvertEscapeToNoEscape(
      NormalDifferentiableFunctionTypeComponent::JVP);
    SILValue convertedVJP = createConvertEscapeToNoEscape(
      NormalDifferentiableFunctionTypeComponent::VJP);

    Optional<std::pair<SILValue, SILValue>> derivativeFunctions;
    if (convertedJVP && convertedVJP)
      derivativeFunctions = std::make_pair(convertedJVP, convertedVJP);

    auto *newDFI = Builder.createDifferentiableFunction(
      DFI->getLoc(), DFI->getParameterIndices(), DFI->getResultIndices(),
      originalNoEscape, derivativeFunctions);
    assert(newDFI->getType() == Cvt->getType() &&
           "New `@differentiable` function instruction should have same type "
           "as the old `convert_escape_to_no_escape` instruction");
    return newDFI;
  }

  return nullptr;
}

SILInstruction *
SILCombiner::visitConvertFunctionInst(ConvertFunctionInst *cfi) {
  // If this conversion only changes substitutions, then rewrite applications
  // of the converted function as applications of the original.
  //
  // (full_apply (convert_function[only_converts_substitutions] x)) => (full_apply x)
  // (partial_apply (convert_function[only_converts_substitutions] x)) => (convert_function (partial_apply x))
  //
  // TODO: We could generalize this to handle other ABI-compatible cases, by
  // inserting the necessary casts around the arguments.
  if (cfi->onlyConvertsSubstitutions()) {
    SmallVector<Operand *, 32> worklist(cfi->getUses());
    while (!worklist.empty()) {
      auto *use = worklist.pop_back_val();
      auto *user = use->getUser();

      // Look through begin_borrow and copy_value.
      if (isa<BeginBorrowInst>(user) || isa<CopyValueInst>(user)) {
        for (auto result : user->getResults())
          for (auto *resultUse : result->getUses())
            worklist.push_back(resultUse);
        continue;
      }

      if (!isa<ApplySite>(user) || use->getOperandNumber() != 0)
        continue;

      if (auto fas = FullApplySite::isa(user)) {
        // For full apply sites, we only need to replace the `convert_function`
        // with the original value.
        //
        // OWNERSHIP DISCUSSION: We know that cfi is forwarding, so we know that
        // if cfi is not owned, then we know that cfi->getOperand() must be
        // valid at applySite and also that the applySite does not consume a
        // value. In such a case, just perform the change and continue.
        SILValue newValue = cfi->getOperand();
        if (newValue->getOwnershipKind() != OwnershipKind::Owned &&
            newValue->getOwnershipKind() != OwnershipKind::Guaranteed) {
          getInstModCallbacks().setUseValue(use, newValue);
          fas.setSubstCalleeType(newValue->getType().castTo<SILFunctionType>());
          continue;
        }

        // Otherwise, we need to use the OwnershipReplaceSingleUseHelper since
        // we have been looking through ownership forwarding insts and newValue
        // may be a value with a different lifetime from our original value
        // beyond the initial base value.
        OwnershipReplaceSingleUseHelper helper(ownershipFixupContext, use,
                                               newValue);
        if (!helper)
          continue;
        helper.perform();
        fas.setSubstCalleeType(newValue->getType().castTo<SILFunctionType>());
        continue;
      }

      // If this is a partial_apply, insert a convert_function back to the
      // original result type.
      auto *pa = dyn_cast<PartialApplyInst>(user);
      if (!pa)
        continue;

      auto partialApplyTy = pa->getType();
      if (!hasOwnership()) {
        SILBuilderWithScope localBuilder(std::next(pa->getIterator()), Builder);
        SmallVector<SILValue, 4> args(pa->getArguments().begin(),
                                      pa->getArguments().end());

        auto newPA = Builder.createPartialApply(
            pa->getLoc(), cfi->getOperand(), pa->getSubstitutionMap(), args,
            pa->getFunctionType()->getCalleeConvention());
        auto newConvert = Builder.createConvertFunction(pa->getLoc(), newPA,
                                                        partialApplyTy, false);
        replaceInstUsesWith(*pa, newConvert);
        eraseInstFromFunction(*pa);
        continue;
      }

      OwnershipRAUWHelper checkRAUW(ownershipFixupContext, pa,
                                    cfi->getOperand());
      if (!checkRAUW)
        continue;

      SmallVector<SILValue, 4> args(pa->getArguments().begin(),
                                    pa->getArguments().end());
      auto newValue =
          makeCopiedValueAvailable(cfi->getOperand(), pa->getParent());

      SILBuilderWithScope localBuilder(std::next(pa->getIterator()), Builder);
      auto *newPA = localBuilder.createPartialApply(
          pa->getLoc(), newValue, pa->getSubstitutionMap(), args,
          pa->getFunctionType()->getCalleeConvention());
      if (!use->isLifetimeEnding()) {
        localBuilder.emitDestroyValueOperation(pa->getLoc(), newValue);
      }
      auto *newConvert = localBuilder.createConvertFunction(
          pa->getLoc(), newPA, partialApplyTy, false);
      // We need to end the lifetime of the convert_function/partial_apply since
      // the helper assumes that ossa is correct upon input.
      localBuilder.emitDestroyValueOperation(pa->getLoc(), newConvert);
      // 'newConvert' may have different ownership than then 'cfi'. newConvert
      // is always owned, while 'cfi' may have been guaranteed. OSSA-RAUW
      // validity depends on the ownership kind. Reinstantiate
      // OwnershipRAUWHelper to verify that it is still valid
      // (a very fast check in this case).
      OwnershipRAUWHelper(ownershipFixupContext, pa, newConvert).perform();
    }
  }

  // (convert_function (convert_function x)) => (convert_function x)
  if (auto *subCFI = dyn_cast<ConvertFunctionInst>(cfi->getOperand())) {
    // We handle the case of an identity conversion in inst simplify, so if we
    // see this pattern then we know that we don't have a round trip and thus
    // should just bypass the intermediate conversion.
    if (cfi->getForwardingOwnershipKind() != OwnershipKind::Owned) {
      cfi->getOperandRef().set(subCFI->getOperand());
      // Return cfi to show we changed it.
      return cfi;
    }

    // If we have an owned value, we can only perform this optimization if the
    // convert_function is in the same block to ensure that we know we will
    // eliminate the convert_function. Otherwise we may be breaking up a
    // forwarding chain in favor of additional ARC traffic which isn't
    // canonical.
    SingleBlockOwnedForwardingInstFolder folder(*this, cfi);
    if (folder.add(subCFI))
      return std::move(folder).optimizeWithSetValue(subCFI->getOperand());
  }

  // Push conversion instructions inside `differentiable_function`. This
  // unblocks more optimizations.
  //
  // Before:
  // %x = differentiable_function(%orig, %jvp, %vjp)
  // %y = convert_function %x
  //
  // After:
  // %orig' = convert_function %orig
  // %jvp' = convert_function %jvp
  // %vjp' = convert_function %vjp
  // %y = differentiable_function(%orig', %jvp', %vjp')
  if (auto *DFI = dyn_cast<DifferentiableFunctionInst>(cfi->getOperand())) {
    auto createConvertFunctionOfComponent =
      [&](NormalDifferentiableFunctionTypeComponent extractee) {
        if (!DFI->hasExtractee(extractee))
          return SILValue();
        
        auto operand = DFI->getExtractee(extractee);
        auto convertInstType =
            cfi->getType().castTo<SILFunctionType>();
        auto convertedComponentFnType =
            convertInstType->getDifferentiableComponentType(
                extractee, Builder.getModule());
        auto convertedComponentType =
        SILType::getPrimitiveObjectType(convertedComponentFnType);
        return Builder.createConvertFunction(
            operand.getLoc(), operand, convertedComponentType,
            cfi->withoutActuallyEscaping())->getResult(0);
      };
      SILValue convertedOriginal = createConvertFunctionOfComponent(
          NormalDifferentiableFunctionTypeComponent::Original);
      SILValue convertedJVP = createConvertFunctionOfComponent(
          NormalDifferentiableFunctionTypeComponent::JVP);
      SILValue convertedVJP = createConvertFunctionOfComponent(
          NormalDifferentiableFunctionTypeComponent::VJP);
      Optional<std::pair<SILValue, SILValue>> derivativeFunctions;
      if (convertedJVP && convertedVJP)
        derivativeFunctions = std::make_pair(convertedJVP, convertedVJP);
      auto *newDFI = Builder.createDifferentiableFunction(
          DFI->getLoc(), DFI->getParameterIndices(), DFI->getResultIndices(),
          convertedOriginal, derivativeFunctions);
      assert(newDFI->getType() == cfi->getType() &&
             "New `@differentiable` function instruction should have same type "
             "as the old `convert_function` instruction");
      return newDFI;
  }

  // Replace a convert_function that only has refcounting uses with its
  // operand.
  tryEliminateOnlyOwnershipUsedForwardingInst(cfi, getInstModCallbacks());
  return nullptr;
}
