//===--- SILCombinerBuiltinVisitors.cpp -----------------------------------===//
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
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

using namespace swift;
using namespace swift::PatternMatch;

SILInstruction *SILCombiner::optimizeBuiltinCompareEq(BuiltinInst *BI,
                                                      bool NegateResult) {
  // Canonicalize boolean comparisons.
  if (auto OpTy = BI->getArguments()[0]->getType().getAs<BuiltinIntegerType>())
    if (OpTy->isFixedWidth(1))
      // cmp_eq %X, -1 -> xor (cmp_eq %X, 0), -1
      if (!NegateResult) {
        if (auto *ILOp = dyn_cast<IntegerLiteralInst>(BI->getArguments()[1]))
          if (ILOp->getValue().isAllOnes()) {
            auto X = BI->getArguments()[0];
            SILValue One(ILOp);
            SILValue Zero(
                Builder.createIntegerLiteral(BI->getLoc(), BI->getType(), 0));
            SILValue Inverted(Builder.createBuiltin(
                BI->getLoc(), BI->getName(), BI->getType(), {}, {X, Zero}));
            auto *Xor = Builder.createBuiltinBinaryFunction(
                BI->getLoc(), "xor", BI->getType(), BI->getType(),
                {Inverted, One});
            replaceInstUsesWith(*BI, Xor);
            return eraseInstFromFunction(*BI);
          }
      }
  IsZeroKind LHS = isZeroValue(BI->getArguments()[0]);
  IsZeroKind RHS = isZeroValue(BI->getArguments()[1]);

  // Can't handle unknown values.
  if (LHS == IsZeroKind::Unknown) {
    return nullptr;
  }

  // Canonicalize i1_const == X to X == i1_const.
  // Canonicalize i1_const != X to X != i1_const.
  if (RHS == IsZeroKind::Unknown) {
    auto *CanonI =
        Builder.createBuiltin(BI->getLoc(), BI->getName(), BI->getType(), {},
                              {BI->getArguments()[1], BI->getArguments()[0]});
    replaceInstUsesWith(*BI, CanonI);
    return eraseInstFromFunction(*BI);
  }

  // Can't handle non-zero ptr values.
  if (LHS == IsZeroKind::NotZero && RHS == IsZeroKind::NotZero)
    return nullptr;

  // Set to true if both sides are zero. Set to false if only one side is zero.
  bool Val = (LHS == RHS) ^ NegateResult;

  return Builder.createIntegerLiteral(BI->getLoc(), BI->getType(),
                                      APInt(1, Val));
}

SILInstruction *SILCombiner::optimizeBuiltinIsConcrete(BuiltinInst *BI) {
  if (BI->getOperand(0)->getType().hasArchetype())
    return nullptr;

  return Builder.createIntegerLiteral(BI->getLoc(), BI->getType(), 1);
}

/// Replace
/// \code
///   %b = builtin "COWBufferForReading" %r
///   %bb = begin_borrow %b
///   %a = ref_element_addr %bb
///   ... use %a ...
///   end_borrow %bb
/// \endcode
/// with
/// \code
///   %bb = begin_borrow %r
///   %a = ref_element_addr [immutable] %r
///   ... use %b ...
///   end_borrow %bb
/// \endcode
/// The same for ref_tail_addr.
SILInstruction *
SILCombiner::optimizeBuiltinCOWBufferForReadingOSSA(BuiltinInst *bi) {
  SmallVector<BorrowedValue, 32> accumulatedBorrowedValues;

  // A helper that performs our main loop to look through uses. It ensures
  // that we do not need to fill up the useWorklist on the first iteration.
  for (auto *use : bi->getUses()) {
    // See if we have a borrowing operand that we can find a local borrowed
    // value for. In such a case, we stash that borrowed value so that we can
    // use it to find interior pointer operands.
    if (auto operand = BorrowingOperand(use)) {
      if (operand.isReborrow())
        return nullptr;
      auto bv = operand.getBorrowIntroducingUserResult();
      accumulatedBorrowedValues.push_back(bv);
      continue;
    }

    // Otherwise, look for instructions that we know are uses that we can
    // ignore.
    auto *user = use->getUser();

    // Debug instructions are safe.
    if (user->isDebugInstruction())
      continue;

    // copy_value, destroy_value are safe due to our checking of the
    // instruction use list for safety.
    if (isa<DestroyValueInst>(user) || isa<CopyValueInst>(user))
      continue;

    // An instruction we don't understand, bail.
    return nullptr;
  }

  // Now that we know that we have a case we support, use our stashed
  // BorrowedValues to find all interior pointer operands into this copy of our
  // COWBuffer and mark them as immutable.
  //
  // NOTE: We currently only use nested int ptr operands instead of extended int
  // ptr operands since we do not want to look through reborrows and thus lose
  // dominance.
  while (!accumulatedBorrowedValues.empty()) {
    auto bv = accumulatedBorrowedValues.pop_back_val();
    bv.visitNestedInteriorPointerOperands(
        [&](InteriorPointerOperand intPtrOperand) {
          switch (intPtrOperand.kind) {
          case InteriorPointerOperandKind::Invalid:
            llvm_unreachable("Invalid int pointer kind?!");
          case InteriorPointerOperandKind::RefElementAddr:
            cast<RefElementAddrInst>(intPtrOperand->getUser())->setImmutable();
            return;
          case InteriorPointerOperandKind::RefTailAddr:
            cast<RefTailAddrInst>(intPtrOperand->getUser())->setImmutable();
            return;
          case InteriorPointerOperandKind::OpenExistentialBox:
          case InteriorPointerOperandKind::ProjectBox:
          case InteriorPointerOperandKind::StoreBorrow:
            // Can not mark this immutable.
            return;
          }
        });
  }

  OwnershipRAUWHelper helper(ownershipFixupContext, bi, bi->getOperand(0));
  assert(helper && "COWBufferForReading always has an owned arg/owned result");
  helper.perform();
  return nullptr;
}

/// Replace
/// \code
///   %b = builtin "COWBufferForReading" %r
///   %a = ref_element_addr %b
/// \endcode
/// with
/// \code
///   %a = ref_element_addr [immutable] %r
/// \endcode
/// The same for ref_tail_addr.
SILInstruction *
SILCombiner::optimizeBuiltinCOWBufferForReadingNonOSSA(BuiltinInst *bi) {
  auto useIter = bi->use_begin();
  while (useIter != bi->use_end()) {
    auto nextIter = std::next(useIter);
    SILInstruction *user = useIter->getUser();
    SILValue ref = bi->getOperand(0);
    switch (user->getKind()) {
      case SILInstructionKind::RefElementAddrInst: {
        auto *reai = cast<RefElementAddrInst>(user);
        reai->setOperand(ref);
        reai->setImmutable();
        break;
      }
      case SILInstructionKind::RefTailAddrInst: {
        auto *rtai = cast<RefTailAddrInst>(user);
        rtai->setOperand(ref);
        rtai->setImmutable();
        break;
      }
    case SILInstructionKind::DestroyValueInst:
      cast<DestroyValueInst>(user)->setOperand(ref);
      break;
    case SILInstructionKind::StrongReleaseInst:
      cast<StrongReleaseInst>(user)->setOperand(ref);
      break;
    default:
      break;
    }
    useIter = nextIter;
  }

  // If there are unknown users, keep the builtin, and IRGen will handle it.
  if (bi->use_empty())
    return eraseInstFromFunction(*bi);
  return nullptr;
}

SILInstruction *
SILCombiner::optimizeBuiltinCOWBufferForReading(BuiltinInst *BI) {
  if (hasOwnership())
    return optimizeBuiltinCOWBufferForReadingOSSA(BI);
  return optimizeBuiltinCOWBufferForReadingNonOSSA(BI);
}

static unsigned getTypeWidth(SILType Ty) {
  if (auto BuiltinIntTy = Ty.getAs<BuiltinIntegerType>()) {
    if (BuiltinIntTy->isFixedWidth()) {
      return BuiltinIntTy->getFixedWidth();
    }
  }
  return 0;
}

SILInstruction *SILCombiner::optimizeBuiltinTruncOrBitCast(BuiltinInst *I) {
  assert(I->getBuiltinInfo().ID == BuiltinValueKind::TruncOrBitCast &&
         "BuiltinInst is not Trunc");
  SILValue Op = I->getArguments()[0];
  SILValue Source;
  if (match(Op, m_ZExtOrBitCast(m_SILValue(Source)))) {
    SILType ResultType = I->getType();
    SILType SourceType = Source->getType();
    SILType SourceTargetType = Op->getType();
    unsigned ResultTypeWidth = getTypeWidth(ResultType);
    unsigned SourceTypeWidth = getTypeWidth(SourceType);
    unsigned SourceTargetTypeWidth = getTypeWidth(SourceTargetType);
    if (ResultTypeWidth == 0 || SourceTypeWidth == 0 ||
        SourceTargetTypeWidth == 0) {
      // Not all types involved have fixed width
      return nullptr;
    }
    if (SourceTargetTypeWidth <= SourceTypeWidth) {
      return nullptr;
    }
    if (ResultTypeWidth < SourceTypeWidth) {
      return nullptr;
    }
    // Reach here if and only if:
    // SourceTargetTypeWidth > SourceTypeWidth and ResultTypeWidth >=
    // SourceTypeWidth
    auto *NI = Builder.createBuiltinBinaryFunctionWithTwoOpTypes(
        I->getLoc(), "zextOrBitCast", Source->getType(), ResultType, ResultType,
        Source);
    replaceInstUsesWith(*I, NI);
    return eraseInstFromFunction(*I);
  }
  return nullptr;
}

SILInstruction *SILCombiner::optimizeBuiltinZextOrBitCast(BuiltinInst *I) {
  assert(I->getBuiltinInfo().ID == BuiltinValueKind::ZExtOrBitCast &&
         "BuiltinInst is not ZExt");
  SILValue Op = I->getArguments()[0];
  SILValue Source;
  if (match(Op, m_ZExtOrBitCast(m_SILValue(Source)))) {
    SILType ResultType = I->getType();
    // We can't extend to a size *smaller* than the source.
    // As such, this transformation will always maintain correctness
    auto *NI = Builder.createBuiltinBinaryFunctionWithTwoOpTypes(
        I->getLoc(), "zextOrBitCast", Source->getType(), ResultType, ResultType,
        Source);
    replaceInstUsesWith(*I, NI);
    return eraseInstFromFunction(*I);
  }
  // Optimize a sequence of conversion of an builtin integer to and from
  // BridgeObject. This sequence appears in the String implementation.
  if (auto *BO2W = dyn_cast<BridgeObjectToWordInst>(Op)) {
    if (auto *V2BO = dyn_cast<ValueToBridgeObjectInst>(BO2W->getOperand())) {
      if (auto *SI = dyn_cast<StructInst>(V2BO->getOperand())) {
        if (SI->getNumOperands() == 1 && SI->getOperand(0)->getType() == I->getType()) {
          replaceInstUsesWith(*I, SI->getOperand(0));
          return eraseInstFromFunction(*I);
        }
      }
    }
  }
  return nullptr;
}

/// Optimize builtins which receive the same value in their first and second
/// operand.
static SILInstruction *optimizeBuiltinWithSameOperands(SILBuilder &Builder,
                                                       BuiltinInst *I,
                                                       SILCombiner *C) {
  // Handle all builtins which can be optimized.
  // We have to take special care about floating point operations because of
  // potential NaN values. E.g. ordered equal FCMP_OEQ(Nan, Nan) is not true.
  switch (I->getBuiltinInfo().ID) {
      
  // Replace the uses with one of the (identical) operands.
  case BuiltinValueKind::And:
  case BuiltinValueKind::Or: {
    // We cannot just _return_ the operand because it is not necessarily an
    // instruction. It can be an argument.
    SILValue Op = I->getOperand(0);
    C->replaceInstUsesWith(*I, Op);
    break;
  }

  // Return 0 or false.
  case BuiltinValueKind::Sub:
  case BuiltinValueKind::SRem:
  case BuiltinValueKind::URem:
  case BuiltinValueKind::Xor:
  case BuiltinValueKind::ICMP_NE:
  case BuiltinValueKind::ICMP_SLT:
  case BuiltinValueKind::ICMP_SGT:
  case BuiltinValueKind::ICMP_ULT:
  case BuiltinValueKind::ICMP_UGT:
  case BuiltinValueKind::FCMP_ONE:
    if (auto Ty = I->getType().getAs<BuiltinIntegerType>()) {
      // FIXME: Should also use SILBuilderWithScope?
      return Builder.createIntegerLiteral(I->getLoc(), I->getType(),
                                          APInt(Ty->getGreatestWidth(), 0));
    }
    break;
      
  // Return 1 or true.
  case BuiltinValueKind::ICMP_EQ:
  case BuiltinValueKind::ICMP_SLE:
  case BuiltinValueKind::ICMP_SGE:
  case BuiltinValueKind::ICMP_ULE:
  case BuiltinValueKind::ICMP_UGE:
  case BuiltinValueKind::FCMP_UEQ:
  case BuiltinValueKind::FCMP_UGE:
  case BuiltinValueKind::FCMP_ULE:
  case BuiltinValueKind::SDiv:
  case BuiltinValueKind::UDiv:
    if (auto Ty = I->getType().getAs<BuiltinIntegerType>()) {
      // FIXME: Should also use SILBuilderWithScope?
      return Builder.createIntegerLiteral(I->getLoc(), I->getType(),
                                          APInt(Ty->getGreatestWidth(), 1));
    }
    break;

  // Return 0 in a tuple.
  case BuiltinValueKind::SSubOver:
  case BuiltinValueKind::USubOver: {
    SILType Ty = I->getType();
    SILType IntTy = Ty.getTupleElementType(0);
    SILType BoolTy = Ty.getTupleElementType(1);
    SILBuilderWithScope B(I);
    SILValue Elements[] = {
      B.createIntegerLiteral(I->getLoc(), IntTy, /* Result */ 0),
      B.createIntegerLiteral(I->getLoc(), BoolTy, /* Overflow */ 0)
    };
    return B.createTuple(I->getLoc(), Ty, Elements);
  }

  // Replace the type check with 'true'.
  case BuiltinValueKind::IsSameMetatype:
    return Builder.createIntegerLiteral(I->getLoc(), I->getType(), true);

  default:
    break;
  }
  return nullptr;
}

/// Match an index pointer that is fed by a sizeof(T)*Distance offset.
static IndexRawPointerInst *
matchSizeOfMultiplication(SILValue I, MetatypeInst *RequiredType,
                          BuiltinInst *&TruncOrBitCast, SILValue &Ptr,
                          SILValue &Distance) {
  auto *Res = dyn_cast<IndexRawPointerInst>(I);
  if (!Res)
    return nullptr;

  SILValue Dist;
  MetatypeInst *StrideType;
  if (match(Res->getOperand(1),
            m_ApplyInst(
                BuiltinValueKind::TruncOrBitCast,
                m_TupleExtractOperation(
                    m_ApplyInst(
                        BuiltinValueKind::SMulOver, m_SILValue(Dist),
                        m_ApplyInst(BuiltinValueKind::ZExtOrBitCast,
                                    m_ApplyInst(BuiltinValueKind::Strideof,
                                                m_MetatypeInst(StrideType)))),
                    0))) ||
      match(Res->getOperand(1),
            m_ApplyInst(
                BuiltinValueKind::TruncOrBitCast,
                m_TupleExtractOperation(
                    m_ApplyInst(
                        BuiltinValueKind::SMulOver,
                        m_ApplyInst(BuiltinValueKind::ZExtOrBitCast,
                                    m_ApplyInst(BuiltinValueKind::Strideof,
                                                m_MetatypeInst(StrideType))),
                        m_SILValue(Dist)),
                    0)))) {
    if (StrideType != RequiredType)
      return nullptr;
    TruncOrBitCast = cast<BuiltinInst>(Res->getOperand(1));
    Distance = Dist;
    Ptr = Res->getOperand(0);
    return Res;
  }
  return nullptr;
}

/// Given an index_raw_pointer Ptr, size_of(Metatype) * Distance create an
/// address_to_pointer (index_addr ptr, Distance : $*Metatype) : $RawPointer
/// instruction.
static SILValue createIndexAddrFrom(IndexRawPointerInst *I,
                                    MetatypeInst *Metatype,
                                    BuiltinInst *TruncOrBitCast,
                                    SILValue Ptr, SILValue Distance,
                                    SILType RawPointerTy,
                                    SILBuilder &Builder) {
  Builder.setCurrentDebugScope(I->getDebugScope());

  CanType InstanceType =
    Metatype->getType().castTo<MetatypeType>().getInstanceType();

  // index_raw_pointer's address type is currently always strict.
  auto *NewPTAI = Builder.createPointerToAddress(
    I->getLoc(), Ptr, SILType::getPrimitiveAddressType(InstanceType),
    /*isStrict*/ true, /*isInvariant*/ false);

  auto *DistanceAsWord =
      Builder.createBuiltin(I->getLoc(), TruncOrBitCast->getName(),
                             TruncOrBitCast->getType(), {}, Distance);

  auto *NewIAI = Builder.createIndexAddr(I->getLoc(), NewPTAI, DistanceAsWord,
                                         /*needsStackProtection=*/ false);
  auto *NewATPI =
    Builder.createAddressToPointer(I->getLoc(), NewIAI, RawPointerTy,
                                   /*needsStackProtection=*/ false);
  return NewATPI;
}

/// Optimize an array operation that has (index_raw_pointer b, sizeof(T) * Dist)
/// operands into one that use index_addr as operands.
SILInstruction *optimizeBuiltinArrayOperation(BuiltinInst *I,
                                              SILBuilder &Builder) {
  if (I->getNumOperands() < 3)
    return nullptr;

  // Take something like this:
  //   %stride = Builtin.strideof(T) * %distance
  //   %ptr' = index_raw_pointer %ptr, %stride
  //     = builtin "takeArrayFrontToBack"<Int>(%metatype, %ptr', ...

  // And convert it to this:
  //   %addr = pointer_to_address %ptr, strict $*T
  //   %result = index_addr %addr, %distance
  //   %ptr' = address_to_pointer result : $RawPointer
  //     = builtin "takeArrayFrontToBack"<Int>(%metatype, %ptr', ...

  auto *Metatype = dyn_cast<MetatypeInst>(I->getOperand(0));
  if (!Metatype)
    return nullptr;

  SILValue Ptr;
  SILValue Distance;
  BuiltinInst *TruncOrBitCast;
  SILValue NewOp1 = I->getOperand(1), NewOp2 = I->getOperand(2);

  // Try to replace the first pointer operand.
  auto *IdxRawPtr1 = matchSizeOfMultiplication(I->getOperand(1), Metatype,
                                               TruncOrBitCast, Ptr, Distance);
  if (IdxRawPtr1)
    NewOp1 = createIndexAddrFrom(IdxRawPtr1, Metatype, TruncOrBitCast, Ptr,
                                 Distance, NewOp1->getType(), Builder);

  // Try to replace the second pointer operand.
  auto *IdxRawPtr2 = matchSizeOfMultiplication(I->getOperand(2), Metatype,
                                               TruncOrBitCast, Ptr, Distance);
  if (IdxRawPtr2)
    NewOp2 = createIndexAddrFrom(IdxRawPtr2, Metatype, TruncOrBitCast, Ptr,
                                 Distance, NewOp2->getType(), Builder);

  if (NewOp1 != I->getOperand(1) || NewOp2 != I->getOperand(2)) {
    SmallVector<SILValue, 5> NewOpds;
    for (auto OldOpd : I->getArguments())
      NewOpds.push_back(OldOpd);
    NewOpds[1] = NewOp1;
    NewOpds[2] = NewOp2;
    return Builder.createBuiltin(I->getLoc(), I->getName(), I->getType(),
                                 I->getSubstitutions(), NewOpds);
  }
  return nullptr;
}

/// Get operands of a binary bitop builtin where one operand is an integer
/// literal.
static bool getBitOpArgs(BuiltinInst *BI, SILValue &op, APInt &bits) {
  OperandValueArrayRef Args = BI->getArguments();
  if (auto *IL = dyn_cast<IntegerLiteralInst>(Args[0])) {
    op = Args[1];
    bits = IL->getValue();
    return true;
  }
  if (auto *IL = dyn_cast<IntegerLiteralInst>(Args[1])) {
    op = Args[0];
    bits = IL->getValue();
    return true;
  }
  return false;
}

/// Optimizes binary bit operations. Optimizations for "and":
///   x & 0 -> 0
///   x & ~0 -> x
///   (x & c1) & c2 -> x & (c1 & c2)
/// The same optimizations are done for "or" and "xor".
template <typename CombineFunc, typename NeutralFunc, typename ZeroFunc>
SILInstruction *optimizeBitOp(BuiltinInst *BI,
                              CombineFunc combine,
                              NeutralFunc isNeutral,
                              ZeroFunc isZero,
                              SILBuilder &Builder,
                              SILCombiner *C) {
  SILValue firstOp;
  APInt bits;
  if (!getBitOpArgs(BI, firstOp, bits))
    return nullptr;

  // Combine all bits of consecutive bit operations, e.g. ((op & c1) & c2) & c3
  SILValue op = firstOp;
  BuiltinInst *Prev;
  APInt prevBits;
  while ((Prev = dyn_cast<BuiltinInst>(op)) &&
         Prev->getBuiltinInfo().ID == BI->getBuiltinInfo().ID &&
         getBitOpArgs(Prev, op, prevBits)) {
    combine(bits, prevBits);
  }
  if (isNeutral(bits)) {
    // The bit operation has no effect, e.g. x | 0 -> x
    C->replaceInstUsesWith(*BI, op);
    return BI;
  }

  if (isZero(bits))
    // The bit operation yields to a constant, e.g. x & 0 -> 0
    return Builder.createIntegerLiteral(BI->getLoc(), BI->getType(), bits);
  
  if (op != firstOp) {
    // We combined multiple bit operations to a single one,
    // e.g. (x & c1) & c2 -> x & (c1 & c2)
    auto *newLI = Builder.createIntegerLiteral(BI->getLoc(), BI->getType(),
                                                bits);
    return Builder.createBuiltin(BI->getLoc(), BI->getName(), BI->getType(),
                                 BI->getSubstitutions(),
                                 { op, newLI });
  }
  return nullptr;
}

/// Returns a 64-bit integer constant if \p op is an integer_literal instruction
/// with a value which fits into 64 bits.
static llvm::Optional<uint64_t> getIntConst(SILValue op) {
  if (auto *ILI = dyn_cast<IntegerLiteralInst>(op)) {
    if (ILI->getValue().getActiveBits() <= 64)
      return ILI->getValue().getZExtValue();
  }
  return llvm::None;
}

/// Optimize the bit extract of a string object. Example in SIL pseudo-code,
/// omitting the type-conversion instructions:
///
///    %0 = string_literal
///    %1 = integer_literal 0x8000000000000000
///    %2 = builtin "stringObjectOr_Int64" (%0, %1)
///    %3 = integer_literal 0x4000000000000000
///    %4 = builtin "and_Int64" (%2, %3)
///
/// optimizes to an integer_literal of 0.
SILInstruction *SILCombiner::optimizeStringObject(BuiltinInst *BI) {
  assert(BI->getBuiltinInfo().ID == BuiltinValueKind::And);
  auto AndOp = getIntConst(BI->getArguments()[1]);
  if (!AndOp)
    return nullptr;

  uint64_t andBits = AndOp.value();

  // TODO: It's bad that we have to hardcode the payload bit mask here.
  // Instead we should introduce builtins (or instructions) to extract the
  // payload and extra bits, respectively.
  const uint64_t payloadBits = 0x00ffffffffffffffll;
  if ((andBits & payloadBits) != 0)
    return nullptr;

  uint64_t setBits = 0;
  SILValue val = BI->getArguments()[0];
  while (val->getKind() != ValueKind::StringLiteralInst) {
    switch (val->getKind()) {
      // Look through all the type conversion and projection instructions.
      case ValueKind::StructExtractInst:
      case ValueKind::UncheckedTrivialBitCastInst:
      case ValueKind::ValueToBridgeObjectInst:
        val = cast<SingleValueInstruction>(val)->getOperand(0);
        break;
      case ValueKind::StructInst: {
        auto *SI = cast<StructInst>(val);
        if (SI->getNumOperands() != 1)
          return nullptr;
        val = SI->getOperand(0);
        break;
      }
      case ValueKind::BuiltinInst: {
        auto *B = cast<BuiltinInst>(val);
        switch (B->getBuiltinInfo().ID) {
          case BuiltinValueKind::StringObjectOr:
            // Note that it is a requirement that the or'd bits of the left
            // operand are initially zero.
            if (auto opVal = getIntConst(B->getArguments()[1])) {
              setBits |= opVal.value();
            } else {
              return nullptr;
            }
            LLVM_FALLTHROUGH;
          case BuiltinValueKind::ZExtOrBitCast:
          case BuiltinValueKind::PtrToInt:
            val = B->getArguments()[0];
            break;
          default:
            return nullptr;
        }
        break;
      }
      default:
        return nullptr;
    }
  }
  return Builder.createIntegerLiteral(BI->getLoc(), BI->getType(),
                                      setBits & andBits);
}

SILInstruction *SILCombiner::visitBuiltinInst(BuiltinInst *I) {
  if (I->getBuiltinInfo().ID == BuiltinValueKind::CanBeObjCClass)
    return optimizeBuiltinCanBeObjCClass(I, Builder);
  if (I->getBuiltinInfo().ID == BuiltinValueKind::IsConcrete)
    return optimizeBuiltinIsConcrete(I);
  if (I->getBuiltinInfo().ID == BuiltinValueKind::COWBufferForReading)
    return optimizeBuiltinCOWBufferForReading(I);
  if (I->getBuiltinInfo().ID == BuiltinValueKind::TakeArrayFrontToBack ||
      I->getBuiltinInfo().ID == BuiltinValueKind::TakeArrayBackToFront ||
      I->getBuiltinInfo().ID == BuiltinValueKind::TakeArrayNoAlias ||
      I->getBuiltinInfo().ID == BuiltinValueKind::CopyArray ||
      I->getBuiltinInfo().ID == BuiltinValueKind::AssignCopyArrayNoAlias ||
      I->getBuiltinInfo().ID == BuiltinValueKind::AssignCopyArrayFrontToBack ||
      I->getBuiltinInfo().ID == BuiltinValueKind::AssignCopyArrayBackToFront ||
      I->getBuiltinInfo().ID == BuiltinValueKind::AssignTakeArray)
    return optimizeBuiltinArrayOperation(I, Builder);

  if (I->getBuiltinInfo().ID == BuiltinValueKind::TruncOrBitCast) {
    return optimizeBuiltinTruncOrBitCast(I);
  }
  if (I->getBuiltinInfo().ID == BuiltinValueKind::ZExtOrBitCast) {
    return optimizeBuiltinZextOrBitCast(I);
  }

  if (I->getNumOperands() >= 2 && I->getOperand(0) == I->getOperand(1)) {
    // It's a builtin which has the same value in its first and second operand.
    auto *Replacement = optimizeBuiltinWithSameOperands(Builder, I, this);
    if (Replacement)
      return Replacement;
  }

  // Optimize this case for unsigned and equality comparisons:
  //   cmp_*_T . (zext U->T x, zext U->T y)
  //      => cmp_*_T (x, y)
  switch (I->getBuiltinInfo().ID) {
  case BuiltinValueKind::ICMP_ULT: {
    if (auto *ILOp = dyn_cast<IntegerLiteralInst>(I->getArguments()[0])) {
      if (ILOp->getValue().isMaxValue()) {
        auto *Zero = Builder.createIntegerLiteral(I->getLoc(), I->getType(), 0);
        replaceInstUsesWith(*I, Zero);
        return eraseInstFromFunction(*I);
      }
    }
  }
    LLVM_FALLTHROUGH;
  case BuiltinValueKind::ICMP_ULE:
  case BuiltinValueKind::ICMP_UGE:
  case BuiltinValueKind::ICMP_UGT:
  case BuiltinValueKind::ICMP_EQ:
  case BuiltinValueKind::ICMP_NE: {
    SILValue LCast, RCast;
    if (match(I->getArguments()[0],
              m_ApplyInst(BuiltinValueKind::ZExtOrBitCast,
                          m_SILValue(LCast))) &&
        match(I->getArguments()[1],
              m_ApplyInst(BuiltinValueKind::ZExtOrBitCast,
                          m_SILValue(RCast))) &&
        LCast->getType() == RCast->getType()) {

      auto *NewCmp = Builder.createBuiltinBinaryFunction(
          I->getLoc(), getBuiltinName(I->getBuiltinInfo().ID),
          LCast->getType(), I->getType(), {LCast, RCast});

      I->replaceAllUsesWith(NewCmp);
      replaceInstUsesWith(*I, NewCmp);
      return eraseInstFromFunction(*I);
    }
    break;
  }
  case BuiltinValueKind::And:
    if (SILInstruction *optimized = optimizeStringObject(I))
      return optimized;

    return optimizeBitOp(I,
      [](APInt &left, const APInt &right) { left &= right; }    /* combine */,
      [](const APInt &i) -> bool { return i.isAllOnes(); }      /* isNeutral */,
      [](const APInt &i) -> bool { return i.isMinValue(); }     /* isZero */,
      Builder, this);
  case BuiltinValueKind::Or:
    return optimizeBitOp(I,
      [](APInt &left, const APInt &right) { left |= right; }    /* combine */,
      [](const APInt &i) -> bool { return i.isMinValue(); }     /* isNeutral */,
      [](const APInt &i) -> bool { return i.isAllOnes(); }      /* isZero */,
      Builder, this);
  case BuiltinValueKind::Xor:
    return optimizeBitOp(I,
      [](APInt &left, const APInt &right) { left ^= right; } /* combine */,
      [](const APInt &i) -> bool { return i.isMinValue(); }  /* isNeutral */,
      [](const APInt &i) -> bool { return false; }           /* isZero */,
      Builder, this);
  case BuiltinValueKind::DestroyArray: {
    SubstitutionMap Substs = I->getSubstitutions();
    // Check if the element type is a trivial type.
    if (Substs.getReplacementTypes().size() == 1) {
      Type ElemType = Substs.getReplacementTypes()[0];
      auto &SILElemTy = I->getFunction()->getTypeLowering(ElemType);
      // Destroying an array of trivial types is a no-op.
      if (SILElemTy.isTrivial())
        return eraseInstFromFunction(*I);
    }
    break;
  }
  case BuiltinValueKind::CondFailMessage:
    if (auto *SLI = dyn_cast<StringLiteralInst>(I->getOperand(1))) {
      if (SLI->getEncoding() == StringLiteralInst::Encoding::UTF8) {
        Builder.createCondFail(I->getLoc(), I->getOperand(0), SLI->getValue());
        eraseInstFromFunction(*I);
        return nullptr;
      }
    }
    break;
  default:
    break;
  }
  
  if (I->getBuiltinInfo().ID == BuiltinValueKind::ICMP_EQ)
    return optimizeBuiltinCompareEq(I, /*Negate Eq result*/ false);

  if (I->getBuiltinInfo().ID == BuiltinValueKind::ICMP_NE)
    return optimizeBuiltinCompareEq(I, /*Negate Eq result*/ true);

  // Optimize sub(ptrtoint(index_raw_pointer(v, x)), ptrtoint(v)) -> x.
  BuiltinInst *Bytes2;
  IndexRawPointerInst *Indexraw;
  if (I->getNumOperands() == 2 &&
      match(I, m_BuiltinInst(BuiltinValueKind::Sub,
                             m_BuiltinInst(BuiltinValueKind::PtrToInt,
                                           m_IndexRawPointerInst(Indexraw)),
                             m_BuiltinInst(Bytes2)))) {
    if (match(Bytes2,
              m_BuiltinInst(BuiltinValueKind::PtrToInt, m_ValueBase()))) {
      if (Indexraw->getOperand(0) == Bytes2->getOperand(0) &&
          Indexraw->getOperand(1)->getType() == I->getType()) {
        replaceInstUsesWith(*I, Indexraw->getOperand(1));
        return eraseInstFromFunction(*I);
      }
    }
  }

  // Canonicalize multiplication by a stride to be such that the stride is
  // always the second argument.
  if (I->getNumOperands() != 3)
    return nullptr;

  if (match(I, m_ApplyInst(BuiltinValueKind::SMulOver,
                            m_ApplyInst(BuiltinValueKind::Strideof),
                            m_ValueBase(), m_IntegerLiteralInst()))) {
    I->swapOperands(0, 1);
    return I;
  }

  return nullptr;
}
