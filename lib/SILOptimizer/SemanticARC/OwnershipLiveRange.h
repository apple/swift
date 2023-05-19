//===--- OwnershipLiveRange.h ---------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_SEMANTICARC_OWNERSHIPLIVERANGE_H
#define SWIFT_SILOPTIMIZER_SEMANTICARC_OWNERSHIPLIVERANGE_H

#include "swift/SIL/OwnershipUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"

namespace swift {
namespace semanticarc {

/// This class represents an "extended live range" of an owned value. Such a
/// representation includes:
///
/// 1. The owned introducing value.
/// 2. Any forwarding instructions that consume the introduced value
///    (transitively) and then propagate a new owned value.
/// 3. Transitive destroys on the forwarding instructions/destroys on the owned
///    introducing value.
/// 4. Any unknown consuming uses that are not understood by this code.
///
/// This allows for this to be used to convert such a set of uses from using
/// owned ownership to using guaranteed ownership by converting the
/// destroy_value -> end_borrow and "flipping" the ownership of individual
/// forwarding instructions.
///
/// NOTE: We do not look through "phi nodes" in the ownership graph (e.x.: real
/// phi arguments, struct, tuple). Instead we represent those as nodes in a
/// larger phi ownership web, connected via individual OwnershipLiveRange.
class LLVM_LIBRARY_VISIBILITY OwnershipLiveRange {
  /// The value that we are computing the LiveRange for. Expected to be an owned
  /// introducer and not to be forwarding.
  OwnedValueIntroducer introducer;

  /// A vector that we store all of our uses into.
  ///
  /// Some properties of this array are:
  ///
  /// 1. It is only mutated in the constructor of LiveRange.
  ///
  /// 2. destroyingUses, ownershipForwardingUses, and unknownConsumingUses are
  /// views into this array. We store the respective uses in the aforementioned
  /// order. This is why it is important not to mutate consumingUses after we
  /// construct the LiveRange since going from small -> large could invalidate
  /// the uses.
  SmallVector<Operand *, 6> consumingUses;

  /// A list of destroy_values of the live range.
  ///
  /// This is just a view into consuming uses.
  ArrayRef<Operand *> destroyingUses;

  /// A list of forwarding instructions that forward owned ownership, but that
  /// are also able to be converted to guaranteed ownership.
  ///
  /// If we are able to eliminate this LiveRange due to it being from a
  /// guaranteed value, we must flip the ownership of all of these instructions
  /// to guaranteed from owned.
  ///
  /// NOTE: Normally only destroying or consuming uses end the live range. We
  /// copy these transitive uses as well into the consumingUses array since
  /// transitive uses can extend a live range up to an unreachable block without
  /// ultimately being consuming. In such a situation if we did not also store
  /// this into consuming uses, we would not be able to ascertain just using the
  /// "consumingUses" array the true lifetime of the OwnershipLiveRange.
  ///
  /// Corresponds to isOwnershipForwardingInst(...).
  ArrayRef<Operand *> ownershipForwardingUses;

  /// Consuming uses that we were not able to understand as a forwarding
  /// instruction or a destroy_value. These must be passed a strongly control
  /// equivalent +1 value.
  ArrayRef<Operand *> unknownConsumingUses;

public:
  OwnershipLiveRange(SILValue value);
  OwnershipLiveRange(const OwnershipLiveRange &) = delete;
  OwnershipLiveRange &operator=(const OwnershipLiveRange &) = delete;

  enum class HasConsumingUse_t {
    No = 0,
    YesButAllPhiArgs = 1,
    Yes = 2,
  };

  /// Return true if v only has invalidating uses that are destroy_value. Such
  /// an owned value is said to represent a dead "live range".
  ///
  /// Semantically this implies that a value is never passed off as +1 to memory
  /// or another function implying it can be used everywhere at +0.
  HasConsumingUse_t
  hasUnknownConsumingUse(bool assumingFixedPoint = false) const;

  /// Return an array ref to /all/ consuming uses. Will include all 3 sorts of
  /// consuming uses: destroying uses, forwarding consuming uses, and unknown
  /// forwarding instruction.
  ArrayRef<Operand *> getAllConsumingUses() const { return consumingUses; }

  ArrayRef<Operand *> getDestroyingUses() const { return destroyingUses; }

  ArrayRef<Operand *> getUnknownConsumingUses() const {
    return unknownConsumingUses;
  }

private:
  struct OperandToUser;

public:
  using DestroyingInstsRange =
      TransformRange<ArrayRef<Operand *>, OperandToUser>;
  DestroyingInstsRange getDestroyingInsts() const;

  using ConsumingInstsRange =
      TransformRange<ArrayRef<Operand *>, OperandToUser>;
  ConsumingInstsRange getAllConsumingInsts() const;

  /// If this LiveRange has a single destroying use, return that use. Otherwise,
  /// return nullptr.
  Operand *getSingleDestroyingUse() const {
    if (destroyingUses.size() != 1) {
      return nullptr;
    }
    return destroyingUses.front();
  }

  /// If this LiveRange has a single unknown destroying use, return that
  /// use. Otherwise, return nullptr.
  Operand *getSingleUnknownConsumingUse() const {
    if (unknownConsumingUses.size() != 1) {
      return nullptr;
    }
    return unknownConsumingUses.front();
  }

  OwnedValueIntroducer getIntroducer() const { return introducer; }

  ArrayRef<Operand *> getOwnershipForwardingUses() const {
    return ownershipForwardingUses;
  }

  void convertOwnedGeneralForwardingUsesToGuaranteed() &&;

  /// A consuming operation that:
  ///
  /// 1. If \p insertEndBorrows is true inserts end borrows at all
  ///    destroying insts locations.
  ///
  /// 2. Deletes all destroy_values.
  ///
  /// 3. RAUW value with newGuaranteedValue.
  ///
  /// 4. Convert all of the general forwarding instructions from
  ///    @owned -> @guaranteed. "Like Dominoes".
  ///
  /// 5. Leaves all of the unknown consuming users alone. It is up to
  ///    the caller to handle converting their ownership.
  void convertToGuaranteedAndRAUW(SILValue newGuaranteedValue,
                                  InstModCallbacks callbacks) &&;

  /// A consuming operation that in order:
  ///
  /// 1. Converts the phi argument to be guaranteed via setOwnership.
  ///
  /// 2. If this consumes a borrow, insert end_borrows at the relevant
  /// destroy_values.
  ///
  /// 3. Deletes all destroy_values.
  ///
  /// 4. Converts all of the general forwarding instructions from @owned ->
  /// @guaranteed. "Like Dominoes".
  ///
  /// NOTE: This leaves all of the unknown consuming users alone. It is up to
  /// the caller to handle converting their ownership.
  ///
  /// NOTE: This routine leaves inserting begin_borrows for the incoming values
  /// to the caller since those are not part of the LiveRange itself.
  void convertJoinedLiveRangePhiToGuaranteed(
      DeadEndBlocks &deadEndBlocks, ValueLifetimeAnalysis::Frontier &scratch,
      InstModCallbacks callbacks) &&;

  /// Given a new guaranteed value, insert end_borrow for the newGuaranteedValue
  /// at all of our destroy_values in preparation for converting from owned to
  /// guaranteed.
  ///
  /// This is used when converting load [copy] -> load_borrow.
  void insertEndBorrowsAtDestroys(SILValue newGuaranteedValue,
                                  DeadEndBlocks &deadEndBlocks,
                                  ValueLifetimeAnalysis::Frontier &scratch);
};

struct OwnershipLiveRange::OperandToUser {
  OperandToUser() {}

  SILInstruction *operator()(const Operand *use) const {
    auto *nonConstUse = const_cast<Operand *>(use);
    return nonConstUse->getUser();
  }
};

} // namespace semanticarc
} // namespace swift

#endif // SWIFT_SILOPTIMIZER_SEMANTICARC_OWNERSHIPLIVERANGE_H
