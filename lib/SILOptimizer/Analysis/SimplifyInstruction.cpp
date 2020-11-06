//===--- SimplifyInstruction.cpp - Fold instructions ----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// An SSA-peephole analysis. Given a single-value instruction, find an existing
/// equivalent but less costly or more canonical SIL value.
///
/// This analysis must handle 'raw' SIL form. It should be possible to perform
/// the substitution discovered by the analysis without interfering with
/// subsequent diagnostic passes.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-simplify"

#include "swift/SILOptimizer/Analysis/SimplifyInstruction.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/ValueTracking.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"

using namespace swift;
using namespace swift::PatternMatch;

namespace swift {
  class ASTContext;
} // end namespace swift

namespace {
  class InstSimplifier : public SILInstructionVisitor<InstSimplifier, SILValue>{
  public:
    SILValue visitSILInstruction(SILInstruction *I) { return SILValue(); }

    SILValue visitTupleExtractInst(TupleExtractInst *TEI);
    SILValue visitStructExtractInst(StructExtractInst *SEI);
    SILValue visitEnumInst(EnumInst *EI);
    SILValue visitSelectEnumInst(SelectEnumInst *SEI);
    SILValue visitUncheckedEnumDataInst(UncheckedEnumDataInst *UEDI);
    SILValue visitAddressToPointerInst(AddressToPointerInst *ATPI);
    SILValue visitPointerToAddressInst(PointerToAddressInst *PTAI);
    SILValue visitRefToRawPointerInst(RefToRawPointerInst *RRPI);
    SILValue
    visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI);
    SILValue visitUncheckedRefCastInst(UncheckedRefCastInst *OPRI);
    SILValue visitUncheckedAddrCastInst(UncheckedAddrCastInst *UACI);
    SILValue visitStructInst(StructInst *SI);
    SILValue visitTupleInst(TupleInst *SI);
    SILValue visitBuiltinInst(BuiltinInst *AI);
    SILValue visitUpcastInst(UpcastInst *UI);
#define LOADABLE_REF_STORAGE(Name, ...) \
    SILValue visitRefTo##Name##Inst(RefTo##Name##Inst *I); \
    SILValue visit##Name##ToRefInst(Name##ToRefInst *I);
#include "swift/AST/ReferenceStorage.def"
    SILValue visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *UBCI);
    SILValue
    visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *UTBCI);
    SILValue visitEndCOWMutationInst(EndCOWMutationInst *ECM);
    SILValue visitThinFunctionToPointerInst(ThinFunctionToPointerInst *TFTPI);
    SILValue visitPointerToThinFunctionInst(PointerToThinFunctionInst *PTTFI);
    SILValue visitBeginAccessInst(BeginAccessInst *BAI);
    SILValue visitMetatypeInst(MetatypeInst *MTI);

    SILValue simplifyOverflowBuiltin(BuiltinInst *BI);
  };
} // end anonymous namespace

SILValue InstSimplifier::visitStructInst(StructInst *SI) {
  // Ignore empty structs.
  if (SI->getNumOperands() < 1)
    return SILValue();

  // Optimize structs that are generated from struct_extract instructions
  // from the same struct.
  if (auto *Ex0 = dyn_cast<StructExtractInst>(SI->getOperand(0))) {
    // Check that the constructed struct and the extracted struct are of the
    // same type.
    if (SI->getType() != Ex0->getOperand()->getType())
      return SILValue();

    // Check that all of the operands are extracts of the correct kind.
    for (unsigned i = 0, e = SI->getNumOperands(); i < e; ++i) {
      auto *Ex = dyn_cast<StructExtractInst>(SI->getOperand(i));
      // Must be an extract.
      if (!Ex)
        return SILValue();

      // Extract from the same struct as the first extract_inst.
      if (Ex0->getOperand() != Ex->getOperand())
        return SILValue();

      // And the order of the field must be identical to the construction order.
      if (Ex->getFieldIndex() != i)
        return SILValue();
    }

    return Ex0->getOperand();
  }

  return SILValue();
}

SILValue InstSimplifier::visitTupleInst(TupleInst *TI) {
  // Ignore empty tuples.
  if (TI->getNumOperands() < 1)
    return SILValue();

  // Optimize tuples that are generated from tuple_extract instructions
  // from the same tuple.
  if (auto *Ex0 = dyn_cast<TupleExtractInst>(TI->getOperand(0))) {
    // Check that the constructed tuple and the extracted tuple are of the
    // same type.
    if (TI->getType() != Ex0->getOperand()->getType())
      return SILValue();

    // Check that all of the operands are extracts of the correct kind.
    for (unsigned i = 0, e = TI->getNumOperands(); i < e; ++i) {
      auto *Ex = dyn_cast<TupleExtractInst>(TI->getOperand(i));
      // Must be an extract.
      if (!Ex)
        return SILValue();

      // Extract from the same struct as the first extract_inst.
      if (Ex0->getOperand() != Ex->getOperand())
        return SILValue();

      // And the order of the field must be identical to the construction order.
      if (Ex->getFieldIndex() != i)
        return SILValue();
    }

    return Ex0->getOperand();
  }

  return SILValue();
}

SILValue InstSimplifier::visitTupleExtractInst(TupleExtractInst *TEI) {
  // tuple_extract(tuple(x, y), 0) -> x
  if (auto *TheTuple = dyn_cast<TupleInst>(TEI->getOperand()))
    return TheTuple->getElement(TEI->getFieldIndex());

  // tuple_extract(apply([add|sub|...]overflow(x,y)),  0) -> x
  // tuple_extract(apply(checked_trunc(ext(x))), 0) -> x
  if (TEI->getFieldIndex() == 0)
    if (auto *BI = dyn_cast<BuiltinInst>(TEI->getOperand()))
      return simplifyOverflowBuiltin(BI);

  return SILValue();
}

SILValue InstSimplifier::visitStructExtractInst(StructExtractInst *SEI) {
  // struct_extract(struct(x, y), x) -> x
  //
  // struct_extract can only take guaranteed values so the struct must be
  // guaranteed as well.
  if (auto *Struct = dyn_cast<StructInst>(SEI->getOperand()))
    return Struct->getFieldValue(SEI->getField());

  return SILValue();
}

SILValue
InstSimplifier::
visitUncheckedEnumDataInst(UncheckedEnumDataInst *UEDI) {
  // (unchecked_enum_data (enum payload)) -> payload
  if (auto *EI = dyn_cast<EnumInst>(UEDI->getOperand())) {
    if (EI->getElement() != UEDI->getElement())
      return SILValue();

    // unchecked_enum_data/enum can take owned values. Since we do not know if
    // out caller is going to get rid of the underlying value, do not return the
    // SILValue() if we have an owned value.
    if (UEDI->getOwnershipKind() == ValueOwnershipKind::Owned)
      return SILValue();

    assert(EI->hasOperand() &&
           "Should only get data from an enum with payload.");
    return EI->getOperand();
  }

  return SILValue();
}

// Simplify:
//   %1 = unchecked_enum_data %0 : $Optional<C>, #Optional.Some!enumelt
//   %2 = enum $Optional<C>, #Optional.Some!enumelt, %1 : $C
// to %0 since we are building the same enum.
static SILValue simplifyEnumFromUncheckedEnumData(EnumInst *EI) {
  assert(EI->hasOperand() && "Expected an enum with an operand!");

  auto *UEDI = dyn_cast<UncheckedEnumDataInst>(EI->getOperand());
  if (!UEDI || UEDI->getElement() != EI->getElement())
    return SILValue();
  
  SILValue EnumOp = UEDI->getOperand();
  
  // Same enum elements don't necessarily imply same enum types.
  // Enum types may be different if the enum is generic, e.g.
  // E<Int>.Case and E<Double>.Case.
  SILType OriginalEnum = EnumOp->getType();
  SILType NewEnum = EI->getType();

  if (OriginalEnum != NewEnum)
    return SILValue();

  // Unless we know that our caller will eliminate the unchecked_enum_data
  // itself, we can only perform this simplification if we have a value without
  // owned ownership.
  if (EnumOp.getOwnershipKind() == ValueOwnershipKind::Owned)
    return SILValue();

  return EnumOp;
}

SILValue InstSimplifier::visitSelectEnumInst(SelectEnumInst *SEI) {

  auto *EI = dyn_cast<EnumInst>(SEI->getEnumOperand());
  if (EI && EI->getType() == SEI->getEnumOperand()->getType()) {
    // Simplify a select_enum on an enum instruction.
    //   %27 = enum $Optional<Int>, #Optional.Some!enumelt, %20 : $Int
    //   %28 = integer_literal $Builtin.Int1, -1
    //   %29 = integer_literal $Builtin.Int1, 0
    //   %30 = select_enum %27 : $Optional<Int>, case #Optional.None!enumelt: %28,
    //                                         case #Optional.Some!enumelt: %29
    // We will return %29.
    return SEI->getCaseResult(EI->getElement());
  }

  return SILValue();
}

SILValue InstSimplifier::visitEnumInst(EnumInst *EI) {
  if (EI->hasOperand()) {
    auto Result = simplifyEnumFromUncheckedEnumData(EI);
    if (Result)
      return Result;

    // switch_enum %e : $EnumTy, case %casex: bbX,...
    // bbX(%arg):
    // enum $EnumTy, EnumTy::casex, %arg
    // ->
    // replace enum $EnumTy, EnumTy::casex, %arg by %e

    auto Op = EI->getOperand();

    auto *EnumArg = dyn_cast<SILArgument>(Op);
    if (!EnumArg)
      return SILValue();

    SILBasicBlock *EnumBlock = EI->getParent();
    if (EnumArg->getParent() != EnumBlock)
      return SILValue();

    auto *Pred = EnumBlock->getSinglePredecessorBlock();
    if (!Pred)
      return SILValue();

    auto *SEI = dyn_cast<SwitchEnumInst>(Pred->getTerminator());
    if (!SEI)
      return SILValue();

    auto Case = SEI->getUniqueCaseForDestination(EI->getParent());

    // NOTE: We can not perform this optimization if the underlying value is
    // owned. But if it is guaranteed, we may be able to do it. Since without
    // ownership all values have None, we can just check for Owned here.
    if (SEI->getOperand().getOwnershipKind() == ValueOwnershipKind::Owned)
      return SILValue();

    if (Case && Case.getPtrOrNull() == EI->getElement() &&
        SEI->getOperand()->getType() == EI->getType()) {
      return SEI->getOperand();
    }

    return SILValue();
  }

  // Simplify enum insts to the value from a switch_enum when possible, e.g.
  // for
  //   switch_enum %0 : $Bool, case #Bool.true!enumelt: bb1
  // bb1:
  //   %1 = enum $Bool, #Bool.true!enumelt
  //
  // we'll return %0. We can only do this if we have a guaranteed value since
  // otherwise the switch_enum will have consumed %0 so the value will no longer
  // be valid.
  auto *BB = EI->getParent();
  auto *Pred = BB->getSinglePredecessorBlock();
  if (!Pred)
    return SILValue();

  if (auto *SEI = dyn_cast<SwitchEnumInst>(Pred->getTerminator())) {
    if (EI->getType() != SEI->getOperand()->getType())
      return SILValue();

    if (SEI->getOperand().getOwnershipKind() == ValueOwnershipKind::Owned)
      return SILValue();

    if (EI->getElement() == SEI->getUniqueCaseForDestination(BB).getPtrOrNull())
      return SEI->getOperand();
  }

  return SILValue();
}

SILValue InstSimplifier::visitAddressToPointerInst(AddressToPointerInst *ATPI) {
  // (address_to_pointer (pointer_to_address x [strict])) -> x
  // The 'strict' flag is only relevant for instructions that access memory;
  // the moment the address is cast back to a pointer, it no longer matters.
  if (auto *PTAI = dyn_cast<PointerToAddressInst>(ATPI->getOperand()))
    if (PTAI->getType() == ATPI->getOperand()->getType())
      return PTAI->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitPointerToAddressInst(PointerToAddressInst *PTAI) {
  // (pointer_to_address strict (address_to_pointer x)) -> x
  //
  // If this address is not strict, then it cannot be replaced by an address
  // that may be strict.
  //
  // When ownership is enabled, we can only perform this if the underlying
  // address of our operand is either not an interior pointer or if it is an
  // interior pointer if all uses of the pointer to address are with in the
  // borrow scope that the interior pointer belongs to.
  if (auto *ATPI = dyn_cast<AddressToPointerInst>(PTAI->getOperand())) {
    if (ATPI->getOperand()->getType() == PTAI->getType() && PTAI->isStrict()) {
      if (!PTAI->getFunction()->hasOwnership())
        return ATPI->getOperand();

      // If we do not have an interior pointer operand, we can just use this
      // without issue. If we do have an interior pointer operand, we have to
      // make sure that we are not adding a new interior pointer address to a
      // borrow scope. This can happen since the address to pointer can be used
      // to escape an address out of a borrow scope. If the pointer to address
      // that rematerializes the address is outside the borrow scope and has
      // uses outside of the borrow scope, we could introduce an ownership
      // violation.
      //
      // TODO: We need to be able to exhaustively traverse all uses of the
      // resulting address to determine if all are within the borrow scope. This
      // requires using the access scope. Instead of doing that, we just bail
      // here.
      SILValue baseAddr =
          getUnderlyingObjectStoppingAtMixedProjections(ATPI->getOperand());
      auto *svi = dyn_cast<SingleValueInstruction>(baseAddr);
      if (!svi || svi->getNumOperands() == 0 ||
          !InteriorPointerOperand::get(&svi->getAllOperands()[0]))
        return ATPI->getOperand();
      return SILValue();
    }
  }

  return SILValue();
}

SILValue InstSimplifier::visitRefToRawPointerInst(RefToRawPointerInst *RefToRaw) {
  // Perform the following simplification:
  //
  // (ref_to_raw_pointer (raw_pointer_to_ref x)) -> x
  //
  // *NOTE* We don't need to check types here.
  if (auto *RawToRef = dyn_cast<RawPointerToRefInst>(&*RefToRaw->getOperand()))
    return RawToRef->getOperand();

  return SILValue();
}

SILValue
InstSimplifier::
visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI) {
  // (UCCI downcast (upcast x #type1 to #type2) #type2 to #type1) -> x
  if (auto *upcast = dyn_cast<UpcastInst>(UCCI->getOperand()))
    if (UCCI->getType() == upcast->getOperand()->getType())
      return upcast->getOperand();

  return SILValue();
}

/// If the only use of a cast is a destroy, just destroy the cast operand.
static SILValue simplifyDeadCast(SingleValueInstruction *Cast) {
  for (Operand *op : Cast->getUses()) {
    switch (op->getUser()->getKind()) {
      case SILInstructionKind::DestroyValueInst:
      case SILInstructionKind::StrongReleaseInst:
      case SILInstructionKind::StrongRetainInst:
        break;
      default:
        return SILValue();
    }
  }
  return Cast->getOperand(0);
}

SILValue
InstSimplifier::
visitUncheckedRefCastInst(UncheckedRefCastInst *OPRI) {
  // (unchecked-ref-cast Y->X (unchecked-ref-cast x X->Y)) -> x
  if (auto *ROPI = dyn_cast<UncheckedRefCastInst>(&*OPRI->getOperand()))
    if (ROPI->getOperand()->getType() == OPRI->getType())
      return ROPI->getOperand();

  // (unchecked-ref-cast Y->X (upcast x X->Y)) -> x
  if (auto *UI = dyn_cast<UpcastInst>(OPRI->getOperand()))
    if (UI->getOperand()->getType() == OPRI->getType())
      return UI->getOperand();

  // (unchecked-ref-cast Y->X (open_existential_ref x X->Y)) -> x
  if (auto *OER = dyn_cast<OpenExistentialRefInst>(OPRI->getOperand()))
    if (OER->getOperand()->getType() == OPRI->getType())
      return OER->getOperand();

  // (unchecked-ref-cast X->X x) -> x
  if (OPRI->getOperand()->getType() == OPRI->getType())
    return OPRI->getOperand();

  // (destroy_value (unchecked_ref_cast x)) -> destroy_value x
  return simplifyDeadCast(OPRI);
}

SILValue
InstSimplifier::
visitUncheckedAddrCastInst(UncheckedAddrCastInst *UACI) {
  // (unchecked-addr-cast Y->X (unchecked-addr-cast x X->Y)) -> x
  if (auto *OtherUACI = dyn_cast<UncheckedAddrCastInst>(&*UACI->getOperand()))
    if (OtherUACI->getOperand()->getType() == UACI->getType())
      return OtherUACI->getOperand();

  // (unchecked-addr-cast X->X x) -> x
  if (UACI->getOperand()->getType() == UACI->getType())
    return UACI->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitUpcastInst(UpcastInst *UI) {
  // (upcast Y->X (unchecked-ref-cast x X->Y)) -> x
  if (auto *URCI = dyn_cast<UncheckedRefCastInst>(UI->getOperand()))
    if (URCI->getOperand()->getType() == UI->getType())
      return URCI->getOperand();

  // (destroy_value (upcast x)) -> destroy_value x
  return simplifyDeadCast(UI);
}

#define LOADABLE_REF_STORAGE(Name, ...)                                        \
  SILValue InstSimplifier::visitRefTo##Name##Inst(RefTo##Name##Inst *RUI) {    \
    if (RUI->getFunction()->hasOwnership())                                    \
      return SILValue();                                                       \
    if (auto *URI = dyn_cast<Name##ToRefInst>(RUI->getOperand()))              \
      if (URI->getOperand()->getType() == RUI->getType())                      \
        return URI->getOperand();                                              \
    return SILValue();                                                         \
  }                                                                            \
  SILValue InstSimplifier::visit##Name##ToRefInst(Name##ToRefInst *URI) {      \
    if (URI->getFunction()->hasOwnership())                                    \
      return SILValue();                                                       \
    if (auto *RUI = dyn_cast<RefTo##Name##Inst>(URI->getOperand()))            \
      if (RUI->getOperand()->getType() == URI->getType())                      \
        return RUI->getOperand();                                              \
    return SILValue();                                                         \
  }
#include "swift/AST/ReferenceStorage.def"

SILValue
InstSimplifier::
visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *UTBCI) {
  // (unchecked_trivial_bit_cast X->X x) -> x
  if (UTBCI->getOperand()->getType() == UTBCI->getType())
    return UTBCI->getOperand();

  // (unchecked_trivial_bit_cast Y->X (unchecked_trivial_bit_cast X->Y x)) -> x
  if (auto *Op = dyn_cast<UncheckedTrivialBitCastInst>(UTBCI->getOperand()))
    if (Op->getOperand()->getType() == UTBCI->getType())
      return Op->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitEndCOWMutationInst(EndCOWMutationInst *ECM) {
  // (destroy_value (end_cow_mutation x)) -> destroy_value x
  return simplifyDeadCast(ECM);
}

SILValue
InstSimplifier::
visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *UBCI) {
  // TODO: Implement this for ownership.
  //
  // The problem is unchecked_bitwise_cast introduces unowned ownership which
  // our original value may not have. So this is not a simple simplification.
  if (!UBCI->getFunction()->hasOwnership())
    return SILValue();

  // (unchecked_bitwise_cast X->X x) -> x
  if (UBCI->getOperand()->getType() == UBCI->getType())
    return UBCI->getOperand();

  // A round-trip cast implies X and Y have the same size:
  // (unchecked_bitwise_cast Y->X (unchecked_bitwise_cast X->Y x)) -> x
  if (auto *Op = dyn_cast<UncheckedBitwiseCastInst>(UBCI->getOperand()))
    if (Op->getOperand()->getType() == UBCI->getType())
      return Op->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitThinFunctionToPointerInst(ThinFunctionToPointerInst *TFTPI) {
  // (thin_function_to_pointer (pointer_to_thin_function x)) -> x
  if (auto *PTTFI = dyn_cast<PointerToThinFunctionInst>(TFTPI->getOperand()))
    if (PTTFI->getOperand()->getType() == TFTPI->getType())
      return PTTFI->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitPointerToThinFunctionInst(PointerToThinFunctionInst *PTTFI) {
  // (pointer_to_thin_function (thin_function_to_pointer x)) -> x
  if (auto *TFTPI = dyn_cast<ThinFunctionToPointerInst>(PTTFI->getOperand()))
    if (TFTPI->getOperand()->getType() == PTTFI->getType())
      return TFTPI->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitBeginAccessInst(BeginAccessInst *BAI) {
  // Remove "dead" begin_access.
  if (llvm::all_of(BAI->getUses(), [](Operand *operand) -> bool {
        return isIncidentalUse(operand->getUser());
      })) {
    return BAI->getOperand();
  }
  return SILValue();
}

SILValue InstSimplifier::visitMetatypeInst(MetatypeInst *MI) {
  auto metaType = MI->getType().castTo<MetatypeType>();
  auto instanceType = metaType.getInstanceType();
  // Tuple, Struct, and Enum MetatypeTypes have a single value.
  // If this metatype is already passed as an argument reuse it to enable
  // downstream CSE/SILCombine optimizations.
  // Note: redundant metatype instructions are already handled by CSE.
  if (isa<TupleType>(instanceType)
      || instanceType.getStructOrBoundGenericStruct()
      || instanceType.getEnumOrBoundGenericEnum()) {
    for (SILArgument *argument : MI->getFunction()->getArguments()) {
      if (argument->getType().getASTType() == metaType &&
          argument->getType().isObject())
        return argument;
    }
  }
  return SILValue();
}

static SILValue simplifyBuiltin(BuiltinInst *BI) {

  switch (BI->getBuiltinInfo().ID) {
    case BuiltinValueKind::IntToPtr:
      if (auto *OpBI = dyn_cast<BuiltinInst>(BI->getOperand(0))) {
        if (OpBI->getBuiltinInfo().ID == BuiltinValueKind::PtrToInt) {
          return OpBI->getOperand(0);
        }
      }
      return SILValue();
    default:
      break;
  }

  const IntrinsicInfo &Intrinsic = BI->getIntrinsicInfo();

  switch (Intrinsic.ID) {
  default:
    // TODO: Handle some of the llvm intrinsics here.
    return SILValue();
  case llvm::Intrinsic::not_intrinsic:
    break;
  case llvm::Intrinsic::expect:
    // If we have an expect optimizer hint with a constant value input,
    // there is nothing left to expect so propagate the input, i.e.,
    //
    // apply(expect, constant, _) -> constant.
    if (auto *Literal = dyn_cast<IntegerLiteralInst>(BI->getArguments()[0]))
      return Literal;
    return SILValue();
  }

  // Otherwise, it should be one of the builtin functions.
  OperandValueArrayRef Args = BI->getArguments();
  const BuiltinInfo &Builtin = BI->getBuiltinInfo();

  switch (Builtin.ID) {
  default: break;

  case BuiltinValueKind::ZExtOrBitCast:
  case BuiltinValueKind::SExtOrBitCast: {
    const SILValue &Op = Args[0];
    // [s|z]extOrBitCast_N_N(x) -> x
    if (Op->getType() == BI->getType())
      return Op;
  }
  break;

  case BuiltinValueKind::TruncOrBitCast: {
    const SILValue &Op = Args[0];
    SILValue Result;
    // truncOrBitCast_N_N(x) -> x
    if (Op->getType() == BI->getType())
      return Op;
    // trunc(extOrBitCast(x)) -> x
    if (match(Op, m_ExtOrBitCast(m_SILValue(Result)))) {
      // Truncated back to the same bits we started with.
      if (Result->getType() == BI->getType())
        return Result;
    }

    return SILValue();
  }

  case BuiltinValueKind::Xor: {
    SILValue val1, val2, val3;
    // xor (xor (val1, val2), val3) == val1
    if (BI->getNumOperands() == 2 &&
        (match(BI,
               m_BuiltinInst(BuiltinValueKind::Xor,
                             m_BuiltinInst(BuiltinValueKind::Xor,
                                           m_SILValue(val1), m_SILValue(val2)),
                             m_SILValue(val3))) ||
         match(BI, m_BuiltinInst(BuiltinValueKind::Xor, m_SILValue(val3),
                                 m_BuiltinInst(BuiltinValueKind::Xor,
                                               m_SILValue(val1),
                                               m_SILValue(val2)))))) {

      if (val2 == val3)
        return val1;
      if (val1 == val3)
        return val2;
      if (val1 == val2)
        return val3;
    }
  }
  break;
  case BuiltinValueKind::Shl:
  case BuiltinValueKind::AShr:
  case BuiltinValueKind::LShr:
    auto *RHS = dyn_cast<IntegerLiteralInst>(Args[1]);
    if (RHS && !RHS->getValue()) {
      // Shifting a value by 0 bits is equivalent to the value itself.
      auto LHS = Args[0];
      return LHS;
    }
    break;
  }
  return SILValue();
}

/// Simplify an apply of the builtin canBeClass to either 0 or 1
/// when we can statically determine the result.
SILValue InstSimplifier::visitBuiltinInst(BuiltinInst *BI) {
  return simplifyBuiltin(BI);
}

/// Simplify arithmetic intrinsics with overflow and known identity
/// constants such as 0 and 1.
/// If this returns a value other than SILValue() then the instruction was
/// simplified to a value which doesn't overflow.  The overflow case is handled
/// in SILCombine.
static SILValue simplifyBinaryWithOverflow(BuiltinInst *BI,
                                           llvm::Intrinsic::ID ID) {
  OperandValueArrayRef Args = BI->getArguments();
  assert(Args.size() >= 2);

  const SILValue &Op1 = Args[0];
  const SILValue &Op2 = Args[1];

  auto *IntOp1 = dyn_cast<IntegerLiteralInst>(Op1);
  auto *IntOp2 = dyn_cast<IntegerLiteralInst>(Op2);

  // If both ops are not constants, we cannot do anything.
  // FIXME: Add cases where we can do something, eg, (x - x) -> 0
  if (!IntOp1 && !IntOp2)
    return SILValue();

  // Calculate the result.

  switch (ID) {
  default: llvm_unreachable("Invalid case");
  case llvm::Intrinsic::sadd_with_overflow:
  case llvm::Intrinsic::uadd_with_overflow:
    // 0 + X -> X
    if (match(Op1, m_Zero()))
      return Op2;
    // X + 0 -> X
    if (match(Op2, m_Zero()))
      return Op1;
    return SILValue();
  case llvm::Intrinsic::ssub_with_overflow:
  case llvm::Intrinsic::usub_with_overflow:
    // X - 0 -> X
    if (match(Op2, m_Zero()))
      return Op1;
    return SILValue();
  case llvm::Intrinsic::smul_with_overflow:
  case llvm::Intrinsic::umul_with_overflow:
    // 0 * X -> 0
    if (match(Op1, m_Zero()))
      return Op1;
    // X * 0 -> 0
    if (match(Op2, m_Zero()))
      return Op2;
    // 1 * X -> X
    if (match(Op1, m_One()))
      return Op2;
    // X * 1 -> X
    if (match(Op2, m_One()))
      return Op1;
    return SILValue();
  }
  return SILValue();
}

/// Simplify operations that may overflow. All such operations return a tuple.
/// This function simplifies such operations, but returns only the first
/// element of a tuple. It looks strange at the first glance, but this
/// is OK, because this function is invoked only internally when processing
/// tuple_extract instructions. Therefore the result of this function
/// is used for simplifications like tuple_extract(x, 0) -> simplified(x)
SILValue InstSimplifier::simplifyOverflowBuiltin(BuiltinInst *BI) {
  const IntrinsicInfo &Intrinsic = BI->getIntrinsicInfo();

  // If it's an llvm intrinsic, fold the intrinsic.
  switch (Intrinsic.ID) {
  default:
    return SILValue();
  case llvm::Intrinsic::not_intrinsic:
    break;
  case llvm::Intrinsic::sadd_with_overflow:
  case llvm::Intrinsic::uadd_with_overflow:
  case llvm::Intrinsic::ssub_with_overflow:
  case llvm::Intrinsic::usub_with_overflow:
  case llvm::Intrinsic::smul_with_overflow:
  case llvm::Intrinsic::umul_with_overflow:
    return simplifyBinaryWithOverflow(BI, Intrinsic.ID);
  }

  // Otherwise, it should be one of the builtin functions.
  const BuiltinInfo &Builtin = BI->getBuiltinInfo();

  switch (Builtin.ID) {
  default: break;

  case BuiltinValueKind::UToSCheckedTrunc:
  case BuiltinValueKind::UToUCheckedTrunc:
  case BuiltinValueKind::SToUCheckedTrunc:
  case BuiltinValueKind::SToSCheckedTrunc: {
    SILValue Result;
    // CheckedTrunc(Ext(x)) -> x
    if (match(BI, m_CheckedTrunc(m_Ext(m_SILValue(Result)))))
      if (Result->getType() == BI->getType().getTupleElementType(0))
        if (auto signBit = computeSignBit(Result))
          if (!signBit.getValue())
            return Result;
    }
    break;

      // Check and simplify binary arithmetic with overflow.
#define BUILTIN(id, name, Attrs)
#define BUILTIN_BINARY_OPERATION_WITH_OVERFLOW(id, name, _, attrs, overload) \
case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
      return simplifyBinaryWithOverflow(BI,
                          getLLVMIntrinsicIDForBuiltinWithOverflow(Builtin.ID));

  }
  return SILValue();
}

/// Try to simplify the specified instruction, performing local
/// analysis of the operands of the instruction, without looking at its uses
/// (e.g. constant folding).  If a simpler result can be found, it is
/// returned, otherwise a null SILValue is returned.
///
SILValue swift::simplifyInstruction(SILInstruction *I) {
  return InstSimplifier().visit(I);
}

/// Replace an instruction with a simplified result, including any debug uses,
/// and erase the instruction. If the instruction initiates a scope, do not
/// replace the end of its scope; it will be deleted along with its parent.
///
/// This is a simple transform based on the above analysis.
///
/// Return an iterator to the next (nondeleted) instruction.
SILBasicBlock::iterator swift::replaceAllSimplifiedUsesAndErase(
    SILInstruction *I, SILValue result,
    std::function<void(SILInstruction *)> eraseHandler) {

  auto *SVI = cast<SingleValueInstruction>(I);
  assert(SVI != result && "Cannot RAUW a value with itself");
  SILBasicBlock::iterator nextii = std::next(I->getIterator());

  // Only SingleValueInstructions are currently simplified.
  while (!SVI->use_empty()) {
    Operand *use = *SVI->use_begin();
    SILInstruction *user = use->getUser();
    // Erase the end of scope marker.
    if (isEndOfScopeMarker(user)) {
      if (&*nextii == user)
        ++nextii;
      if (eraseHandler)
        eraseHandler(user);
      else
        user->eraseFromParent();
      continue;
    }
    use->set(result);
  }
  if (eraseHandler)
    eraseHandler(I);
  else
    I->eraseFromParent();

  return nextii;
}

/// Simplify invocations of builtin operations that may overflow.
/// All such operations return a tuple (result, overflow_flag).
/// This function try to simplify such operations, but returns only a
/// simplified first element of a tuple. The overflow flag is not returned
/// explicitly, because this simplification is only possible if there is
/// no overflow. Therefore the overflow flag is known to have a value of 0 if
/// simplification was successful.
/// In case when a simplification is not possible, a null SILValue is returned.
SILValue swift::simplifyOverflowBuiltinInstruction(BuiltinInst *BI) {
  return InstSimplifier().simplifyOverflowBuiltin(BI);
}
