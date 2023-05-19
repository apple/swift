//===--- ConstExpr.cpp - Constant expression evaluator -------------------===//
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

#define DEBUG_TYPE "ConstExpr"

#include "swift/SILOptimizer/Utils/ConstExpr.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/Demangling/Demangle.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILConstants.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/TerminatorUtils.h"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "llvm/ADT/PointerEmbeddedInt.h"
#include "llvm/Support/TrailingObjects.h"

using namespace swift;

static llvm::Optional<SymbolicValue>
evaluateAndCacheCall(SILFunction &fn, SubstitutionMap substitutionMap,
                     ArrayRef<SymbolicValue> arguments, SymbolicValue &result,
                     unsigned &numInstEvaluated, ConstExprEvaluator &evaluator);

// TODO: ConstantTracker in the performance inliner and the
// ConstantFolding.h/cpp files should be subsumed by this, as this is a more
// general framework.

enum class WellKnownFunction {
  // Array.init()
  ArrayInitEmpty,
  // Array._allocateUninitializedArray
  AllocateUninitializedArray,
  // Array._endMutation
  EndArrayMutation,
  // _finalizeUninitializedArray
  FinalizeUninitializedArray,
  // Array.append(_:)
  ArrayAppendElement,
  // String.init()
  StringInitEmpty,
  // String.init(_builtinStringLiteral:utf8CodeUnitCount:isASCII:)
  StringMakeUTF8,
  // static String.append (_: String, _: inout String)
  StringAppend,
  // static String.== infix(_: String)
  StringEquals,
  // String.percentEscapedString.getter
  StringEscapePercent,
  // BinaryInteger.description.getter
  BinaryIntegerDescription,
  // _assertionFailure(_: StaticString, _: StaticString, file: StaticString,...)
  AssertionFailure,
  // A function taking one argument that prints the symbolic value of the
  // argument during constant evaluation. This must only be used for debugging.
  DebugPrint
};

static llvm::Optional<WellKnownFunction> classifyFunction(SILFunction *fn) {
  if (fn->hasSemanticsAttr(semantics::ARRAY_INIT_EMPTY))
    return WellKnownFunction::ArrayInitEmpty;
  if (fn->hasSemanticsAttr(semantics::ARRAY_UNINITIALIZED_INTRINSIC))
    return WellKnownFunction::AllocateUninitializedArray;
  if (fn->hasSemanticsAttr(semantics::ARRAY_END_MUTATION))
    return WellKnownFunction::EndArrayMutation;
  if (fn->hasSemanticsAttr(semantics::ARRAY_FINALIZE_INTRINSIC))
    return WellKnownFunction::FinalizeUninitializedArray;
  if (fn->hasSemanticsAttr(semantics::ARRAY_APPEND_ELEMENT))
    return WellKnownFunction::ArrayAppendElement;
  if (fn->hasSemanticsAttr(semantics::STRING_INIT_EMPTY))
    return WellKnownFunction::StringInitEmpty;
  // There are two string initializers in the standard library with the
  // semantics "string.makeUTF8". They are identical from the perspective of
  // the interpreter. One of those functions is probably redundant and not used.
  if (fn->hasSemanticsAttr(semantics::STRING_MAKE_UTF8))
    return WellKnownFunction::StringMakeUTF8;
  if (fn->hasSemanticsAttr(semantics::STRING_APPEND))
    return WellKnownFunction::StringAppend;
  if (fn->hasSemanticsAttr(semantics::STRING_EQUALS))
    return WellKnownFunction::StringEquals;
  if (fn->hasSemanticsAttr(semantics::STRING_ESCAPE_PERCENT_GET))
    return WellKnownFunction::StringEscapePercent;
  if (fn->hasSemanticsAttr(semantics::BINARY_INTEGER_DESCRIPTION))
    return WellKnownFunction::BinaryIntegerDescription;
  if (fn->hasSemanticsAttrThatStartsWith("programtermination_point"))
    return WellKnownFunction::AssertionFailure;
  // A call to a function with the following semantics annotation will be
  // considered as a DebugPrint operation. The evaluator will print the value
  // of the single argument passed to this function call to the standard error.
  // This functionality must be used only for debugging the evaluator.
  if (fn->hasSemanticsAttrThatStartsWith("constant_evaluator_debug_print"))
    return WellKnownFunction::DebugPrint;
  return None;
}

static bool isReadOnlyFunction(WellKnownFunction function) {
  switch (function) {
  case WellKnownFunction::ArrayInitEmpty:
  case WellKnownFunction::AllocateUninitializedArray:
  case WellKnownFunction::StringInitEmpty:
  case WellKnownFunction::StringMakeUTF8:
  case WellKnownFunction::StringEquals:
  case WellKnownFunction::StringEscapePercent:
  case WellKnownFunction::BinaryIntegerDescription:
    return true;

  case WellKnownFunction::EndArrayMutation:
  case WellKnownFunction::FinalizeUninitializedArray:
  case WellKnownFunction::ArrayAppendElement:
  case WellKnownFunction::StringAppend:
  case WellKnownFunction::AssertionFailure:
  case WellKnownFunction::DebugPrint:
    return false;
  }
}

/// Helper function for creating UnknownReason without a payload.
static SymbolicValue getUnknown(ConstExprEvaluator &evaluator, SILNode *node,
                                UnknownReason::UnknownKind kind) {
  return evaluator.getUnknown(node, UnknownReason::create(kind));
}

//===----------------------------------------------------------------------===//
// ConstExprFunctionState implementation.
//===----------------------------------------------------------------------===//

namespace swift {
/// This type represents the state of computed values within a function
/// as evaluation happens.  A separate instance of this is made for each
/// callee in a call chain to represent the constant values given the set of
/// formal parameters that callee was invoked with.
class ConstExprFunctionState {
  /// This is the evaluator that is computing this function state.  We use it to
  /// allocate space for values and to query the call stack.
  ConstExprEvaluator &evaluator;

  /// If we are analyzing the body of a constexpr function, this is the
  /// function.  This is null for the top-level expression.
  SILFunction *fn;

  /// If we have a function being analyzed, this is the substitutionMap for
  /// the call to it.
  /// substitutionMap specifies a mapping from all of the protocol and type
  /// requirements in the generic signature down to concrete conformances and
  /// concrete types.
  SubstitutionMap substitutionMap;

  /// This keeps track of the number of instructions we've evaluated.  If this
  /// goes beyond the execution cap, then we start returning unknown values.
  unsigned &numInstEvaluated;

  /// This is a state of previously analyzed values, maintained and filled in
  /// by getConstantValue.  This does not hold the memory referred to by SIL
  /// addresses.
  llvm::DenseMap<SILValue, SymbolicValue> calculatedValues;

  /// If a SILValue is not bound to a SymbolicValue in the calculatedValues,
  /// try to compute it recursively by visiting its defining instruction.
  bool recursivelyComputeValueIfNotInState = false;

public:
  ConstExprFunctionState(ConstExprEvaluator &evaluator, SILFunction *fn,
                         SubstitutionMap substitutionMap,
                         unsigned &numInstEvaluated,
                         bool enableTopLevelEvaluation)
      : evaluator(evaluator), fn(fn), substitutionMap(substitutionMap),
        numInstEvaluated(numInstEvaluated),
        recursivelyComputeValueIfNotInState(enableTopLevelEvaluation) {
    assert((!fn || !enableTopLevelEvaluation) &&
           "top-level evaluation must be disabled when evaluating a function"
           " body step by step");
  }

  /// Pretty print the state to stderr.
  void dump() const {
    llvm::errs() << "[ConstExprState: \n";
    llvm::errs() << "   Caller: " << (fn ? fn->getName() : "null") << "\n";
    llvm::errs() << "   evaluatedInstrCount: " << numInstEvaluated << "\n";
    llvm::errs() << "   SubstMap: \n";
    substitutionMap.dump(llvm::errs(), SubstitutionMap::DumpStyle::Full, 6);
    llvm::errs() << "\n   calculatedValues: ";
    for (auto kv : calculatedValues) {
      llvm::errs() << "      " << kv.first << " --> " << kv.second << "\n";
    }
  }

  void setValue(SILValue value, SymbolicValue symVal) {
    calculatedValues.insert({value, symVal});
  }

  /// Return the symbolic value for a SILValue if it is bound in the interpreter
  /// state. If not, return None.
  llvm::Optional<SymbolicValue> lookupValue(SILValue value) {
    auto it = calculatedValues.find(value);
    if (it != calculatedValues.end())
      return it->second;
    return None;
  }

  /// Invariant: Before the call, `calculatedValues` must not contain `addr`
  /// as a key.
  SymbolicValue createMemoryObject(SILValue addr, SymbolicValue initialValue) {
    assert(!calculatedValues.count(addr));
    Type valueType =
        substituteGenericParamsAndSimplify(addr->getType().getASTType());
    auto *memObject = SymbolicValueMemoryObject::create(
        valueType, initialValue, evaluator.getAllocator());
    auto result = SymbolicValue::getAddress(memObject);
    setValue(addr, result);
    return result;
  }

  /// Return the SymbolicValue for the specified SIL value. If the SIL value is
  /// not in \c calculatedValues, try computing the SymbolicValue recursively
  /// if \c recursivelyComputeValueIfNotInState flag is set.
  SymbolicValue getConstantValue(SILValue value);

  /// Evaluate the specified instruction in a flow sensitive way, for use by
  /// the constexpr function evaluator.  This does not handle control flow
  /// statements.
  llvm::Optional<SymbolicValue> evaluateFlowSensitive(SILInstruction *inst);

  /// Evaluate a branch or non-branch instruction and if the evaluation was
  /// successful, return the next instruction from where the evaluation must
  /// continue.
  /// \param instI basic-block iterator pointing to the instruction to evaluate.
  /// \param visitedBlocks basic blocks already visited during evaluation.
  ///   This is used to detect loops.
  /// \returns a pair where the first and second elements are defined as
  /// follows:
  ///   If the evaluation of the instruction is successful, the first element
  ///   is the iterator to the next instruction from the where the evaluation
  ///   must continue. Otherwise, it is None.
  ///
  ///   Second element is None, if the evaluation is successful.
  ///   Otherwise, is an unknown symbolic value that contains the error.
  std::pair<Optional<SILBasicBlock::iterator>, Optional<SymbolicValue>>
  evaluateInstructionAndGetNext(
      SILBasicBlock::iterator instI,
      SmallPtrSetImpl<SILBasicBlock *> &visitedBlocks);

  Type substituteGenericParamsAndSimplify(Type ty);
  CanType substituteGenericParamsAndSimplify(CanType ty) {
    return substituteGenericParamsAndSimplify(Type(ty))->getCanonicalType();
  }
  SymbolicValue computeConstantValue(SILValue value);
  SymbolicValue computeConstantValueBuiltin(BuiltinInst *inst);

  llvm::Optional<SymbolicValue> computeCallResult(ApplyInst *apply);

  llvm::Optional<SymbolicValue> computeOpaqueCallResult(ApplyInst *apply,
                                                        SILFunction *callee);

  llvm::Optional<SymbolicValue>
  computeWellKnownCallResult(ApplyInst *apply, WellKnownFunction callee);

  /// Evaluate a closure creation instruction which is either a partial_apply
  /// instruction or a thin_to_think_function instruction. On success, this
  /// function will bind the \c closureInst parameter to its symbolic value.
  /// On failure, it returns the unknown symbolic value that captures the error.
  llvm::Optional<SymbolicValue>
  evaluateClosureCreation(SingleValueInstruction *closureInst);

  SymbolicValue getSingleWriterAddressValue(SILValue addr);
  SymbolicValue getConstAddrAndLoadResult(SILValue addr);
  SymbolicValue loadAddrValue(SILValue addr, SymbolicValue addrVal);
  llvm::Optional<SymbolicValue> computeFSStore(SymbolicValue storedCst,
                                               SILValue dest);
private:
  llvm::Optional<SymbolicValue>
  initializeAddressFromSingleWriter(SILValue addr);
};
} // namespace swift

/// Simplify the specified type based on knowledge of substitutions if we have
/// any.
Type ConstExprFunctionState::substituteGenericParamsAndSimplify(Type ty) {
  return substitutionMap.empty() ? ty : ty.subst(substitutionMap);
}

/// Const-evaluate `value`, which must not have been computed.
SymbolicValue ConstExprFunctionState::computeConstantValue(SILValue value) {
  assert(!calculatedValues.count(value));

  // If this a trivial constant instruction that we can handle, then fold it
  // immediately.
  if (auto *ili = dyn_cast<IntegerLiteralInst>(value))
    return SymbolicValue::getInteger(ili->getValue(), evaluator.getAllocator());
  if (auto *sli = dyn_cast<StringLiteralInst>(value))
    return SymbolicValue::getString(sli->getValue(), evaluator.getAllocator());

  if (auto *fri = dyn_cast<FunctionRefInst>(value))
    return SymbolicValue::getFunction(fri->getReferencedFunction());

  // If we have a reference to a metatype, constant fold any substitutable
  // types.
  if (auto *mti = dyn_cast<MetatypeInst>(value)) {
    auto metatype = mti->getType().castTo<MetatypeType>();
    auto type = substituteGenericParamsAndSimplify(metatype->getInstanceType())
        ->getCanonicalType();
    return SymbolicValue::getMetatype(type);
  }

  if (auto *tei = dyn_cast<TupleExtractInst>(value)) {
    auto val = getConstantValue(tei->getOperand());
    if (!val.isConstant())
      return val;
    return val.getAggregateMembers()[tei->getFieldIndex()];
  }

  // If this is a struct extract from a fragile type, then we can return the
  // element being extracted.
  if (auto *sei = dyn_cast<StructExtractInst>(value)) {
    auto aggValue = sei->getOperand();
    auto val = getConstantValue(aggValue);
    if (!val.isConstant()) {
      return val;
    }
    assert(val.getKind() == SymbolicValue::Aggregate);
    return val.getAggregateMembers()[sei->getFieldIndex()];
  }

  // If this is an unchecked_enum_data from a fragile type, then we can return
  // the enum case value.
  if (auto *uedi = dyn_cast<UncheckedEnumDataInst>(value)) {
    auto aggValue = uedi->getOperand();
    auto val = getConstantValue(aggValue);
    if (val.isConstant()) {
      assert(val.getKind() == SymbolicValue::EnumWithPayload);
      return val.getEnumPayloadValue();
    }
    // Not a const.
    return val;
  }

  // If this is a destructure_result, then we can return the element being
  // extracted.
  if (isaResultOf<DestructureStructInst>(value) ||
      isaResultOf<DestructureTupleInst>(value)) {
    auto *result = cast<MultipleValueInstructionResult>(value);
    SILValue aggValue = result->getParent()->getOperand(0);
    auto val = getConstantValue(aggValue);
    if (val.isConstant()) {
      assert(val.getKind() == SymbolicValue::Aggregate);
      return val.getAggregateMembers()[result->getIndex()];
    }
    // Not a const.
    return val;
  }

  // TODO: If this is a single element struct, we can avoid creating an
  // aggregate to reduce # allocations.  This is extra silly in the case of zero
  // element tuples.
  if (isa<StructInst>(value) || isa<TupleInst>(value)) {
    auto *inst = cast<SingleValueInstruction>(value);
    SmallVector<SymbolicValue, 4> elts;

    for (unsigned i = 0, e = inst->getNumOperands(); i != e; ++i) {
      auto val = getConstantValue(inst->getOperand(i));
      if (!val.isConstant() && !val.isUnknownDueToUnevaluatedInstructions())
        return val;
      // Unknown values due to unevaluated instructions can be assigned to
      // struct properties as they are not indicative of a fatal error or
      // trap.
      elts.push_back(val);
    }
    CanType structType = value->getType().getASTType();
    return SymbolicValue::getAggregate(
        elts, substituteGenericParamsAndSimplify(structType),
        evaluator.getAllocator());
  }

  // If this is a struct or tuple element addressor, compute a more derived
  // address.
  if (isa<StructElementAddrInst>(value) || isa<TupleElementAddrInst>(value)) {
    auto inst = cast<SingleValueInstruction>(value);
    auto baseAddr = getConstantValue(inst->getOperand(0));
    if (!baseAddr.isConstant())
      return baseAddr;

    SmallVector<unsigned, 4> accessPath;
    auto *memObject = baseAddr.getAddressValue(accessPath);

    // Add our index onto the next of the list.
    unsigned index;
    if (auto sea = dyn_cast<StructElementAddrInst>(inst))
      index = sea->getFieldIndex();
    else
      index = cast<TupleElementAddrInst>(inst)->getFieldIndex();
    accessPath.push_back(index);
    return SymbolicValue::getAddress(memObject, accessPath,
                                     evaluator.getAllocator());
  }

  // If this is a load, then we either have computed the value of the memory
  // already (when analyzing the body of a function in a flow-sensitive
  // fashion), or this is the indirect result of a call.  Either way, we ask for
  // the value of the pointer.  In the former case, this will be the latest
  // value of the memory.  In the latter case, the call must be the only
  // store to the address so that the memory object can be computed by
  // recursively processing the allocation and call instructions in a
  // demand-driven fashion.
  if (auto li = dyn_cast<LoadInst>(value))
    return getConstAddrAndLoadResult(li->getOperand());

  // Try to resolve a witness method against our known conformances.
  if (auto *wmi = dyn_cast<WitnessMethodInst>(value)) {
    auto conf = substitutionMap.lookupConformance(
        wmi->getLookupType(), wmi->getConformance().getRequirement());
    if (conf.isInvalid())
      return getUnknown(evaluator, value,
                        UnknownReason::UnknownWitnessMethodConformance);
    auto &module = wmi->getModule();
    SILFunction *fn =
        module.lookUpFunctionInWitnessTable(conf, wmi->getMember(),
            SILModule::LinkingMode::LinkAll).first;
    // If we were able to resolve it, then we can proceed.
    if (fn)
      return SymbolicValue::getFunction(fn);

    LLVM_DEBUG(llvm::dbgs()
               << "ConstExpr Unresolved witness: " << *value << "\n");
    return getUnknown(evaluator, value, UnknownReason::NoWitnesTableEntry);
  }

  if (auto *builtin = dyn_cast<BuiltinInst>(value))
    return computeConstantValueBuiltin(builtin);

  if (auto *enumVal = dyn_cast<EnumInst>(value)) {
    if (!enumVal->hasOperand())
      return SymbolicValue::getEnum(enumVal->getElement());

    auto payload = getConstantValue(enumVal->getOperand());
    if (!payload.isConstant())
      return payload;
    return SymbolicValue::getEnumWithPayload(enumVal->getElement(), payload,
                                             evaluator.getAllocator());
  }

  // This one returns the address of its enum payload.
  if (auto *dai = dyn_cast<UncheckedTakeEnumDataAddrInst>(value)) {
    auto enumVal = getConstAddrAndLoadResult(dai->getOperand());
    if (!enumVal.isConstant())
      return enumVal;
    return createMemoryObject(value, enumVal.getEnumPayloadValue());
  }

  if (isa<SelectEnumInst>(value) || isa<SelectEnumAddrInst>(value)) {
    SelectEnumInstBase *selectInst = dyn_cast<SelectEnumInst>(value);
    if (!selectInst) {
      selectInst = dyn_cast<SelectEnumAddrInst>(value);
    }

    SILValue enumOperand = selectInst->getEnumOperand();
    SymbolicValue enumValue = isa<SelectEnumInst>(selectInst)
                                  ? getConstantValue(enumOperand)
                                  : getConstAddrAndLoadResult(enumOperand);
    if (!enumValue.isConstant())
      return enumValue;

    assert(enumValue.getKind() == SymbolicValue::Enum ||
           enumValue.getKind() == SymbolicValue::EnumWithPayload);

    SILValue resultOperand =
        selectInst->getCaseResult(enumValue.getEnumValue());
    return getConstantValue(resultOperand);
  }

  // This instruction is a marker that returns its first operand.
  if (auto *bai = dyn_cast<BeginAccessInst>(value))
    return getConstantValue(bai->getOperand());

  // Look through copy_value, begin_borrow, and move_value since the
  // interpreter doesn't model these memory management instructions.
  if (isa<CopyValueInst>(value) || isa<BeginBorrowInst>(value) ||
      isa<MoveValueInst>(value))
    return getConstantValue(cast<SingleValueInstruction>(value)->getOperand(0));

  // Builtin.RawPointer and addresses have the same representation.
  if (auto *p2ai = dyn_cast<PointerToAddressInst>(value))
    return getConstantValue(p2ai->getOperand());

  // Indexing a pointer moves the deepest index of the access path it represents
  // within a memory object. For example, if a pointer p represents the access
  // path [1, 2] within a memory object, p + 1 represents [1, 3]
  if (auto *ia = dyn_cast<IndexAddrInst>(value)) {
    auto index = getConstantValue(ia->getOperand(1));
    if (!index.isConstant())
      return index;
    auto basePtr = getConstantValue(ia->getOperand(0));
    if (basePtr.getKind() != SymbolicValue::Address)
      return basePtr;

    SmallVector<unsigned, 4> accessPath;
    auto *memObject = basePtr.getAddressValue(accessPath);
    assert(!accessPath.empty() && "Can't index a non-indexed address");
    accessPath.back() += index.getIntegerValue().getLimitedValue();
    return SymbolicValue::getAddress(memObject, accessPath,
                                     evaluator.getAllocator());
  }
  
  // `convert_function` instructions that only change substitutions can be
  // looked through to the original function.
  //
  // TODO: Certain covariant or otherwise ABI-compatible conversions should
  // be handled as well.
  if (auto cf = dyn_cast<ConvertFunctionInst>(value)) {
    if (cf->onlyConvertsSubstitutions()) {
      return getConstantValue(cf->getOperand());
    }
  }

  if (auto *convertEscapeInst = dyn_cast<ConvertEscapeToNoEscapeInst>(value))
    return getConstantValue(convertEscapeInst->getOperand());

  LLVM_DEBUG(llvm::dbgs() << "ConstExpr Unknown simple: " << *value << "\n");

  // Otherwise, we don't know how to handle this.
  auto unknownReason = isa<SingleValueInstruction>(value)
                           ? UnknownReason::UnsupportedInstruction
                           : UnknownReason::Default;
  return getUnknown(evaluator, value, unknownReason);
}

SymbolicValue
ConstExprFunctionState::computeConstantValueBuiltin(BuiltinInst *inst) {
  const BuiltinInfo &builtin = inst->getBuiltinInfo();

  // Constant builtins.
  if (inst->getNumOperands() == 0) {
    switch (builtin.ID) {
    default:
      break;
    case BuiltinValueKind::AssertConf:
      return SymbolicValue::getInteger(evaluator.getAssertConfig(), 32);
    }
  }

  // Handle various cases in groups.
  auto invalidOperandValue = [&]() -> SymbolicValue {
    return getUnknown(evaluator, SILValue(inst),
                      UnknownReason::InvalidOperandValue);
  };

  // Unary operations.
  if (inst->getNumOperands() == 1) {
    auto operand = getConstantValue(inst->getOperand(0));
    // TODO: Could add a "value used here" sort of diagnostic.
    if (!operand.isConstant())
      return operand;

    // Implement support for s_to_s_checked_trunc_Int2048_Int64 and other
    // checking integer truncates.  These produce a tuple of the result value
    // and an overflow bit.
    //
    // TODO: We can/should diagnose statically detectable integer overflow
    // errors and subsume the ConstantFolding.cpp mandatory SIL pass.
    auto IntCheckedTruncFn = [&](bool srcSigned,
                                 bool dstSigned) -> SymbolicValue {
      if (operand.getKind() != SymbolicValue::Integer)
        return invalidOperandValue();

      APInt operandVal = operand.getIntegerValue();
      uint32_t srcBitWidth = operandVal.getBitWidth();
      auto dstBitWidth =
          builtin.Types[1]->castTo<BuiltinIntegerType>()->getGreatestWidth();

      // Note that the if the source type is a Builtin.IntLiteral, operandVal
      // could have fewer bits than the destination bit width and may only
      // require a sign extension.
      APInt result = operandVal.sextOrTrunc(dstBitWidth);

      // Determine if there is a overflow.
      if (operandVal.getBitWidth() > dstBitWidth) {
        // Re-extend the value back to its source and check for loss of value.
        APInt reextended =
            dstSigned ? result.sext(srcBitWidth) : result.zext(srcBitWidth);
        bool overflowed = (operandVal != reextended);

        if (!srcSigned && dstSigned)
          overflowed |= result.isSignBitSet();

        if (overflowed)
          return getUnknown(evaluator, SILValue(inst), UnknownReason::Overflow);
      }

      auto &allocator = evaluator.getAllocator();
      // Build the Symbolic value result for our truncated value.
      return SymbolicValue::getAggregate(
          {SymbolicValue::getInteger(result, allocator),
           SymbolicValue::getInteger(APInt(1, false), allocator)},
          inst->getType().getASTType(), allocator);
    };

    switch (builtin.ID) {
    default:
      break;
    case BuiltinValueKind::SToSCheckedTrunc:
      return IntCheckedTruncFn(true, true);
    case BuiltinValueKind::UToSCheckedTrunc:
      return IntCheckedTruncFn(false, true);
    case BuiltinValueKind::SToUCheckedTrunc:
      return IntCheckedTruncFn(true, false);
    case BuiltinValueKind::UToUCheckedTrunc:
      return IntCheckedTruncFn(false, false);

    case BuiltinValueKind::Trunc:
    case BuiltinValueKind::TruncOrBitCast:
    case BuiltinValueKind::ZExt:
    case BuiltinValueKind::ZExtOrBitCast:
    case BuiltinValueKind::SExt:
    case BuiltinValueKind::SExtOrBitCast: {
      if (operand.getKind() != SymbolicValue::Integer)
        return invalidOperandValue();

      unsigned destBitWidth =
          inst->getType().castTo<BuiltinIntegerType>()->getGreatestWidth();

      APInt result = operand.getIntegerValue();
      if (result.getBitWidth() != destBitWidth) {
        switch (builtin.ID) {
        default:
          assert(0 && "Unknown case");
        case BuiltinValueKind::Trunc:
        case BuiltinValueKind::TruncOrBitCast:
          result = result.trunc(destBitWidth);
          break;
        case BuiltinValueKind::ZExt:
        case BuiltinValueKind::ZExtOrBitCast:
          result = result.zext(destBitWidth);
          break;
        case BuiltinValueKind::SExt:
        case BuiltinValueKind::SExtOrBitCast:
          result = result.sext(destBitWidth);
          break;
        }
      }
      return SymbolicValue::getInteger(result, evaluator.getAllocator());
    }
    // The two following builtins are supported only for string constants. This
    // is because this builtin is used by StaticString which is used in
    // preconditions and assertion failures. Supporting this enables the
    // evaluator to handle assertion/precondition failures.
    case BuiltinValueKind::PtrToInt:
    case BuiltinValueKind::IntToPtr: {
      if (operand.getKind() != SymbolicValue::String) {
        return getUnknown(evaluator, SILValue(inst),
                          UnknownReason::UnsupportedInstruction);
      }
      return operand;
    }
    }
  }

  // Binary operations.
  if (inst->getNumOperands() == 2) {
    auto operand0 = getConstantValue(inst->getOperand(0));
    auto operand1 = getConstantValue(inst->getOperand(1));
    if (!operand0.isConstant())
      return operand0;
    if (!operand1.isConstant())
      return operand1;

    auto constFoldIntCompare =
        [&](const std::function<bool(const APInt &, const APInt &)> &fn)
        -> SymbolicValue {
      if (operand0.getKind() != SymbolicValue::Integer ||
          operand1.getKind() != SymbolicValue::Integer)
        return invalidOperandValue();

      auto result = fn(operand0.getIntegerValue(), operand1.getIntegerValue());
      return SymbolicValue::getInteger(APInt(1, result),
                                       evaluator.getAllocator());
    };

#define REQUIRE_KIND(KIND)                                                     \
  if (operand0.getKind() != SymbolicValue::KIND ||                             \
      operand1.getKind() != SymbolicValue::KIND)                               \
    return invalidOperandValue();

    switch (builtin.ID) {
    default:
      break;
#define INT_BINOP(OPCODE, EXPR)                                                \
  case BuiltinValueKind::OPCODE: {                                             \
    REQUIRE_KIND(Integer)                                                      \
    auto l = operand0.getIntegerValue(), r = operand1.getIntegerValue();       \
    return SymbolicValue::getInteger((EXPR), evaluator.getAllocator());        \
  }
      INT_BINOP(Add, l + r)
      INT_BINOP(And, l & r)
      INT_BINOP(AShr, l.ashr(r))
      INT_BINOP(LShr, l.lshr(r))
      INT_BINOP(Or, l | r)
      INT_BINOP(Mul, l * r)
      INT_BINOP(SDiv, l.sdiv(r))
      INT_BINOP(Shl, l << r)
      INT_BINOP(SRem, l.srem(r))
      INT_BINOP(Sub, l - r)
      INT_BINOP(UDiv, l.udiv(r))
      INT_BINOP(URem, l.urem(r))
      INT_BINOP(Xor, l ^ r)
#undef INT_BINOP

#define INT_COMPARE(OPCODE, EXPR)                                              \
  case BuiltinValueKind::OPCODE:                                               \
    REQUIRE_KIND(Integer)                                                      \
    return constFoldIntCompare(                                                \
        [&](const APInt &l, const APInt &r) -> bool { return (EXPR); })
      INT_COMPARE(ICMP_EQ, l == r);
      INT_COMPARE(ICMP_NE, l != r);
      INT_COMPARE(ICMP_SLT, l.slt(r));
      INT_COMPARE(ICMP_SGT, l.sgt(r));
      INT_COMPARE(ICMP_SLE, l.sle(r));
      INT_COMPARE(ICMP_SGE, l.sge(r));
      INT_COMPARE(ICMP_ULT, l.ult(r));
      INT_COMPARE(ICMP_UGT, l.ugt(r));
      INT_COMPARE(ICMP_ULE, l.ule(r));
      INT_COMPARE(ICMP_UGE, l.uge(r));
#undef INT_COMPARE
#undef REQUIRE_KIND

    case BuiltinValueKind::Expect:
      return operand0;
    }
  }

  // Three operand builtins.
  if (inst->getNumOperands() == 3) {
    auto operand0 = getConstantValue(inst->getOperand(0));
    auto operand1 = getConstantValue(inst->getOperand(1));
    auto operand2 = getConstantValue(inst->getOperand(2));
    if (!operand0.isConstant())
      return operand0;
    if (!operand1.isConstant())
      return operand1;
    if (!operand2.isConstant())
      return operand2;

    // Overflowing integer operations like sadd_with_overflow take three
    // operands: the last one is a "should report overflow" bit.
    auto constFoldIntOverflow =
        [&](const std::function<APInt(const APInt &, const APInt &, bool &)>
                &fn) -> SymbolicValue {
      if (operand0.getKind() != SymbolicValue::Integer ||
          operand1.getKind() != SymbolicValue::Integer ||
          operand2.getKind() != SymbolicValue::Integer)
        return invalidOperandValue();

      auto l = operand0.getIntegerValue(), r = operand1.getIntegerValue();
      bool overflowed = false;
      auto result = fn(l, r, overflowed);

      // Return a statically diagnosed overflow if the operation is supposed to
      // trap on overflow.
      if (overflowed && !operand2.getIntegerValue().isNullValue())
        return getUnknown(evaluator, SILValue(inst), UnknownReason::Overflow);

      auto &allocator = evaluator.getAllocator();
      // Build the Symbolic value result for our normal and overflow bit.
      return SymbolicValue::getAggregate(
          {SymbolicValue::getInteger(result, allocator),
           SymbolicValue::getInteger(APInt(1, overflowed), allocator)},
          inst->getType().getASTType(), allocator);
    };

    switch (builtin.ID) {
    default:
      break;

#define INT_OVERFLOW(OPCODE, METHOD)                                           \
  case BuiltinValueKind::OPCODE:                                               \
    return constFoldIntOverflow(                                               \
        [&](const APInt &l, const APInt &r, bool &overflowed) -> APInt {       \
          return l.METHOD(r, overflowed);                                      \
        })
      INT_OVERFLOW(SAddOver, sadd_ov);
      INT_OVERFLOW(UAddOver, uadd_ov);
      INT_OVERFLOW(SSubOver, ssub_ov);
      INT_OVERFLOW(USubOver, usub_ov);
      INT_OVERFLOW(SMulOver, smul_ov);
      INT_OVERFLOW(UMulOver, umul_ov);
#undef INT_OVERFLOW
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "ConstExpr Unknown Builtin: " << *inst << "\n");

  // Otherwise, we don't know how to handle this builtin.
  return getUnknown(evaluator, SILValue(inst),
                    UnknownReason::UnsupportedInstruction);
}

// Handle calls to opaque callees, either by handling them and returning None or
// by returning with a Unknown indicating a failure.
llvm::Optional<SymbolicValue>
ConstExprFunctionState::computeOpaqueCallResult(ApplyInst *apply,
                                                SILFunction *callee) {
  LLVM_DEBUG(llvm::dbgs() << "ConstExpr Opaque Callee: " << *callee << "\n");
  return evaluator.getUnknown(
      apply,
      UnknownReason::createCalleeImplementationUnknown(callee));
}

/// Given a symbolic value representing an instance of StaticString, look into
/// the aggregate and extract the static string value stored inside it.
static Optional<StringRef>
extractStaticStringValue(SymbolicValue staticString) {
  if (staticString.getKind() != SymbolicValue::Aggregate)
    return None;
  ArrayRef<SymbolicValue> staticStringProps =
      staticString.getAggregateMembers();
  if (staticStringProps.empty() ||
      staticStringProps[0].getKind() != SymbolicValue::String)
    return None;
  return staticStringProps[0].getStringValue();
}

static Optional<StringRef>
extractStringOrStaticStringValue(SymbolicValue stringValue) {
  if (stringValue.getKind() == SymbolicValue::String)
    return stringValue.getStringValue();
  return extractStaticStringValue(stringValue);
}

/// If the specified type is a Swift.Array of some element type, then return the
/// element type.  Otherwise, return a null Type.
static Type getArrayElementType(Type ty) {
  if (auto bgst = ty->getAs<BoundGenericStructType>())
    if (bgst->isArray())
      return bgst->getGenericArgs()[0];
  return Type();
}

/// Check if the given type \p ty is a stdlib integer type and if so return
/// whether the type is signed. Returns \c None if \p ty is not a stdlib integer
/// type, \c true if it is a signed integer type and \c false if it is an
/// unsigned integer type.
static Optional<bool> getSignIfStdlibIntegerType(Type ty) {
  if (ty->isInt() || ty->isInt8() || ty->isInt16() || ty->isInt32() ||
      ty->isInt64()) {
    return true;
  }

  if (ty->isUInt() || ty->isUInt8() || ty->isUInt16() || ty->isUInt32() ||
      ty->isUInt64()) {
    return false;
  }

  return None;
}

/// Given a call to a well known function, collect its arguments as constants,
/// fold it, and return None.  If any of the arguments are not constants, marks
/// the call's results as Unknown, and return an Unknown with information about
/// the error.
llvm::Optional<SymbolicValue>
ConstExprFunctionState::computeWellKnownCallResult(ApplyInst *apply,
                                                   WellKnownFunction callee) {
  auto conventions = apply->getSubstCalleeConv();
  switch (callee) {
  case WellKnownFunction::AssertionFailure: {
    SmallString<4> message;
    for (unsigned i = 0, e = apply->getNumArguments(); i < e; ++i) {
      SILValue argument = apply->getArgument(i);
      SymbolicValue argValue = getConstantValue(argument);
      Optional<StringRef> stringOpt =
          extractStringOrStaticStringValue(argValue);
      // The first argument is a prefix that specifies the kind of failure
      // this is.
      if (i == 0) {
        if (stringOpt) {
          message += stringOpt.value();
        } else {
          // Use a generic prefix here, as the actual prefix is not a constant.
          message += "assertion failed";
        }
        continue;
      }
      if (stringOpt) {
        message += ": ";
        message += stringOpt.value();
      }
    }
    return evaluator.getUnknown(
        apply,
        UnknownReason::createTrap(message, evaluator.getAllocator()));
  }
  case WellKnownFunction::ArrayInitEmpty: { // Array.init()
    assert(conventions.getNumDirectSILResults() == 1 &&
           conventions.getNumIndirectSILResults() == 0 &&
           "unexpected Array.init() signature");

    auto typeValue = getConstantValue(apply->getOperand(1));
    if (typeValue.getKind() != SymbolicValue::Metatype) {
      return typeValue.isConstant()
                 ? getUnknown(evaluator, apply,
                              UnknownReason::InvalidOperandValue)
                 : typeValue;
    }
    Type arrayType = typeValue.getMetatypeValue();

    // Create an empty SymbolicArrayStorage and then create a SymbolicArray
    // using it.
    SymbolicValue arrayStorage = SymbolicValue::getSymbolicArrayStorage(
        {}, getArrayElementType(arrayType)->getCanonicalType(),
        evaluator.getAllocator());
    auto arrayVal = SymbolicValue::getArray(arrayType, arrayStorage,
                                            evaluator.getAllocator());
    setValue(apply, arrayVal);
    return None;
  }
  case WellKnownFunction::AllocateUninitializedArray: {
    // This function has this signature:
    //   func _allocateUninitializedArray<Element>(_ builtinCount: Builtin.Word)
    //     -> (Array<Element>, Builtin.RawPointer)
    assert(conventions.getNumParameters() == 1 &&
           conventions.getNumDirectSILResults() == 2 &&
           conventions.getNumIndirectSILResults() == 0 &&
           "unexpected _allocateUninitializedArray signature");

    // Figure out the allocation size.
    auto numElementsSV = getConstantValue(apply->getOperand(1));
    if (!numElementsSV.isConstant())
      return numElementsSV;

    unsigned numElements = numElementsSV.getIntegerValue().getLimitedValue();

    // Allocating uninitialized arrays is supported only in flow-sensitive mode.
    // TODO: the top-level mode in the interpreter should be phased out.
    if (recursivelyComputeValueIfNotInState)
      return getUnknown(evaluator, apply, UnknownReason::Default);

    SmallVector<SymbolicValue, 8> elementConstants;
    // Set array elements to uninitialized state. Subsequent stores through
    // their addresses will initialize the elements.
    elementConstants.assign(numElements, SymbolicValue::getUninitMemory());

    Type resultType =
        substituteGenericParamsAndSimplify(apply->getType().getASTType());
    assert(resultType->is<TupleType>());
    Type arrayType = resultType->castTo<TupleType>()->getElementType(0);
    Type arrayEltType = getArrayElementType(arrayType);
    assert(arrayEltType && "Couldn't understand Swift.Array type?");

    // Create a SymbolicArrayStorage with \c elements and then create a
    // SymbolicArray using it.
    SymbolicValueAllocator &allocator = evaluator.getAllocator();
    SymbolicValue arrayStorage = SymbolicValue::getSymbolicArrayStorage(
        elementConstants, arrayEltType->getCanonicalType(), allocator);
    SymbolicValue array =
        SymbolicValue::getArray(arrayType, arrayStorage, allocator);

    // Construct return value for this call, which is a pair consisting of the
    // address of the first element of the array and the array.
    SymbolicValue storageAddress = array.getAddressOfArrayElement(allocator, 0);
    setValue(apply, SymbolicValue::getAggregate({array, storageAddress},
                                                resultType, allocator));
    return None;
  }
  case WellKnownFunction::EndArrayMutation: {
    // This function has the following signature in SIL:
    //    (@inout Array<Element>) -> ()
    assert(conventions.getNumParameters() == 1 &&
           conventions.getNumDirectSILResults() == 0 &&
           conventions.getNumIndirectSILResults() == 0 &&
           "unexpected Array._endMutation() signature");

    // _endMutation is a no-op.
    return None;
  }
  case WellKnownFunction::FinalizeUninitializedArray: {
    // This function has the following signature in SIL:
    //    (Array<Element>) -> Array<Element>
    assert(conventions.getNumParameters() == 1 &&
           conventions.getNumDirectSILResults() == 1 &&
           conventions.getNumIndirectSILResults() == 0 &&
           "unexpected _finalizeUninitializedArray() signature");

    auto result = getConstantValue(apply->getOperand(1));
    if (!result.isConstant())
      return result;
    // Semantically, it's an identity function.
    setValue(apply, result);
    return None;
  }
  case WellKnownFunction::ArrayAppendElement: {
    // This function has the following signature in SIL:
    //    (@in Element, @inout Array<Element>) -> ()
    assert(conventions.getNumParameters() == 2 &&
           conventions.getNumDirectSILResults() == 0 &&
           conventions.getNumIndirectSILResults() == 0 &&
           "unexpected Array.append(_:) signature");
    // Get the element to be appended which is passed indirectly (@in).
    SymbolicValue element = getConstAddrAndLoadResult(apply->getOperand(1));
    if (!element.isConstant())
      return element;

    // Get the array value. The array is passed @inout and could be a property
    // of a struct.
    SILValue arrayAddress = apply->getOperand(2);
    SymbolicValue arrayValue = getConstAddrAndLoadResult(arrayAddress);
    if (!arrayValue.isConstant())
      return arrayValue;
    if (arrayValue.getKind() != SymbolicValue::Array) {
      return getUnknown(evaluator, apply, UnknownReason::InvalidOperandValue);
    }

    // Create a new array storage by appending the \c element to the existing
    // storage, and create a new array using the new storage.
    SymbolicValue arrayStorage = arrayValue.getStorageOfArray();
    CanType elementType;
    ArrayRef<SymbolicValue> oldElements =
        arrayStorage.getStoredElements(elementType);
    SmallVector<SymbolicValue, 4> newElements(oldElements.begin(),
                                              oldElements.end());
    newElements.push_back(element);

    SymbolicValueAllocator &allocator = evaluator.getAllocator();
    SymbolicValue newStorage = SymbolicValue::getSymbolicArrayStorage(
        newElements, elementType, allocator);
    SymbolicValue newArray = SymbolicValue::getArray(arrayValue.getArrayType(),
                                                     newStorage, allocator);
    computeFSStore(newArray, arrayAddress);
    return None;
  }
  case WellKnownFunction::StringInitEmpty: { // String.init()
    assert(conventions.getNumDirectSILResults() == 1 &&
           conventions.getNumIndirectSILResults() == 0 &&
           "unexpected String.init() signature");
    auto result = SymbolicValue::getString("", evaluator.getAllocator());
    setValue(apply, result);
    return None;
  }
  case WellKnownFunction::StringMakeUTF8: {
    // String.init(_builtinStringLiteral start: Builtin.RawPointer,
    //             utf8CodeUnitCount: Builtin.Word,
    //             isASCII: Builtin.Int1)
    assert(conventions.getNumDirectSILResults() == 1 &&
           conventions.getNumIndirectSILResults() == 0 &&
           conventions.getNumParameters() == 4 && "unexpected signature");
    auto literal = getConstantValue(apply->getOperand(1));
    if (literal.getKind() != SymbolicValue::String) {
      return getUnknown(evaluator, apply, UnknownReason::InvalidOperandValue);
    }
    auto literalVal = literal.getStringValue();

    auto byteCount = getConstantValue(apply->getOperand(2));
    if (byteCount.getKind() != SymbolicValue::Integer ||
        byteCount.getIntegerValue().getLimitedValue() != literalVal.size()) {
      return getUnknown(evaluator, apply, UnknownReason::InvalidOperandValue);
    }
    setValue(apply, literal);
    return None;
  }
  case WellKnownFunction::StringAppend: {
    // static String.append (_: String, _: inout String)
    assert(conventions.getNumDirectSILResults() == 0 &&
           conventions.getNumIndirectSILResults() == 0 &&
           conventions.getNumParameters() == 2 &&
           "unexpected String.append() signature");

    auto otherString = getConstantValue(apply->getOperand(1));
    if (!otherString.isConstant()) {
      return otherString;
    }
    if (otherString.getKind() != SymbolicValue::String) {
      return getUnknown(evaluator, apply, UnknownReason::InvalidOperandValue);
    }

    auto inoutOperand = apply->getOperand(2);
    auto firstString = getConstAddrAndLoadResult(inoutOperand);
    if (!firstString.isConstant()) {
      return firstString;
    }
    if (firstString.getKind() != SymbolicValue::String) {
      return getUnknown(evaluator, apply, UnknownReason::InvalidOperandValue);
    }

    auto result = SmallString<8>(firstString.getStringValue());
    result.append(otherString.getStringValue());
    auto resultVal = SymbolicValue::getString(result, evaluator.getAllocator());
    computeFSStore(resultVal, inoutOperand);
    return None;
  }
  case WellKnownFunction::StringEquals: {
    // static String.== infix(_: String, _: String)
    assert(conventions.getNumDirectSILResults() == 1 &&
           conventions.getNumIndirectSILResults() == 0 &&
           conventions.getNumParameters() == 3 &&
           "unexpected String.==() signature");

    auto firstString = getConstantValue(apply->getOperand(1));
    if (firstString.getKind() != SymbolicValue::String) {
      return getUnknown(evaluator, apply, UnknownReason::InvalidOperandValue);
    }

    auto otherString = getConstantValue(apply->getOperand(2));
    if (otherString.getKind() != SymbolicValue::String) {
      return getUnknown(evaluator, apply, UnknownReason::InvalidOperandValue);
    }

    // The result is a Swift.Bool which is a struct that wraps an Int1.
    int isEqual = firstString.getStringValue() == otherString.getStringValue();
    auto intVal =
        SymbolicValue::getInteger(APInt(1, isEqual), evaluator.getAllocator());
    auto result = SymbolicValue::getAggregate(ArrayRef<SymbolicValue>(intVal),
                                              apply->getType().getASTType(),
                                              evaluator.getAllocator());
    setValue(apply, result);
    return None;
  }
  case WellKnownFunction::StringEscapePercent: {
    // String.percentEscapedString.getter
    assert(conventions.getNumDirectSILResults() == 1 &&
           conventions.getNumIndirectSILResults() == 0 &&
           conventions.getNumParameters() == 1 &&
           "unexpected String.percentEscapedString signature");

    auto stringArgument = getConstantValue(apply->getOperand(1));
    if (!stringArgument.isConstant()) {
      return stringArgument;
    }

    if (stringArgument.getKind() != SymbolicValue::String) {
      return getUnknown(evaluator, apply, UnknownReason::InvalidOperandValue);
    }

    // Replace all precent symbol (%) in the string with double percents (%%)
    StringRef stringVal = stringArgument.getStringValue();
    SmallString<4> percentEscapedString;
    for (auto charElem : stringVal) {
      percentEscapedString.push_back(charElem);
      if (charElem == '%') {
        percentEscapedString.push_back('%');
      }
    }

    auto resultVal = SymbolicValue::getString(percentEscapedString.str(),
                                              evaluator.getAllocator());
    setValue(apply, resultVal);
    return None;
  }
  case WellKnownFunction::BinaryIntegerDescription: {
    // BinaryInteger.description.getter
    assert(conventions.getNumDirectSILResults() == 1 &&
           conventions.getNumIndirectSILResults() == 0 &&
           conventions.getNumParameters() == 1 && apply->hasSubstitutions() &&
           "unexpected BinaryInteger.description.getter signature");
    // Get the type of the argument and check if it is a signed or
    // unsigned integer.
    SILValue integerArgument = apply->getOperand(1);
    CanType argumentType = substituteGenericParamsAndSimplify(
        integerArgument->getType().getASTType());
    Optional<bool> isSignedIntegerType =
        getSignIfStdlibIntegerType(argumentType);
    if (!isSignedIntegerType.has_value()) {
      return getUnknown(evaluator, apply, UnknownReason::InvalidOperandValue);
    }
    // Load the stdlib integer's value and convert it to a string.
    SymbolicValue stdlibIntegerValue =
        getConstAddrAndLoadResult(integerArgument);
    if (!stdlibIntegerValue.isConstant()) {
      return stdlibIntegerValue;
    }
    SymbolicValue builtinIntegerValue =
        stdlibIntegerValue.lookThroughSingleElementAggregates();
    assert(builtinIntegerValue.getKind() == SymbolicValue::Integer &&
           "stdlib integer type must store only a builtin integer");
    APInt integer = builtinIntegerValue.getIntegerValue();
    SmallString<8> integerString;
    isSignedIntegerType.value() ? integer.toStringSigned(integerString)
                                   : integer.toStringUnsigned(integerString);
    SymbolicValue resultVal =
        SymbolicValue::getString(integerString.str(), evaluator.getAllocator());
    setValue(apply, resultVal);
    return None;
  }
  case WellKnownFunction::DebugPrint: {
    assert(apply->getNumArguments() == 1 &&
           "debug_print function must take exactly one argument");
    SILValue argument = apply->getArgument(0);
    SymbolicValue argValue = getConstantValue(argument);
    llvm::errs() << "Debug print output ";
    argValue.print(llvm::errs());
    if (argValue.getKind() != SymbolicValue::Address)
      return None;

    llvm::errs() << "\n  Addressed Memory Object: ";
    SymbolicValueMemoryObject *memObj = argValue.getAddressValueMemoryObject();
    memObj->getValue().print(llvm::errs());
    return None;
  }
  }
  llvm_unreachable("unhandled WellKnownFunction");
}

/// Given a call to a function, determine whether it is a call to a constexpr
/// function.  If so, collect its arguments as constants, fold it and return
/// None.  If not, mark the results as Unknown, and return an Unknown with
/// information about the error.
llvm::Optional<SymbolicValue>
ConstExprFunctionState::computeCallResult(ApplyInst *apply) {
  // Determine the callee.
  auto calleeFn = getConstantValue(apply->getOperand(0));
  if (calleeFn.getKind() != SymbolicValue::Function)
    return getUnknown(evaluator, apply, UnknownReason::InvalidOperandValue);

  SILFunction *callee = calleeFn.getFunctionValue();
  evaluator.recordCalledFunctionIfEnabled(callee);

  // If this is a well-known function, do not step into it.
  if (auto wellKnownFunction = classifyFunction(callee))
    return computeWellKnownCallResult(apply, *wellKnownFunction);

  // Verify that we can fold all of the arguments to the call.
  SmallVector<SymbolicValue, 4> paramConstants;
  for (unsigned i = 0, e = apply->getNumOperands() - 1; i != e; ++i) {
    // If any of the arguments is a non-constant value, then we can't fold this
    // call.
    auto op = apply->getOperand(i + 1);
    SymbolicValue argValue = getConstantValue(op);
    if (!argValue.isConstant()) {
      return evaluator.getUnknown(apply,
                                  UnknownReason::createCallArgumentUnknown(i));
    }
    paramConstants.push_back(argValue);
  }

  // If we reached an external function that hasn't been deserialized yet, make
  // sure to pull it in so we can see its body. If that fails, then we can't
  // analyze the function. Note: pull in everything referenced from another
  // module in case some referenced functions have non-public linkage.
  if (callee->isExternalDeclaration()) {
    apply->getModule().linkFunction(callee, SILModule::LinkingMode::LinkAll);
    if (callee->isExternalDeclaration())
      return computeOpaqueCallResult(apply, callee);
  }

  // Compute the substitution map for the callee, which maps from all of its
  // generic requirements to concrete conformances and concrete types.
  SubstitutionMap calleeSubMap;

  auto calleeFnType = callee->getLoweredFunctionType();
  assert(!calleeFnType->hasSelfParam() ||
         !calleeFnType
                 ->getSelfInstanceType(
                     callee->getModule(),
                     apply->getFunction()->getTypeExpansionContext())
                 ->getClassOrBoundGenericClass() &&
             "class methods are not supported");
  if (calleeFnType->getInvocationGenericSignature()) {
    // Get the substitution map of the call.  This maps from the callee's space
    // into the caller's world. Witness methods require additional work to
    // compute a mapping that is valid for the callee.
    SubstitutionMap callSubMap;

    if (calleeFnType->getRepresentation() ==
        SILFunctionType::Representation::WitnessMethod) {
      auto protocol =
          calleeFnType->getWitnessMethodConformanceOrInvalid().getRequirement();
      // Compute a mapping that maps the Self type of the protocol given by
      // 'requirement' to the concrete type available in the substitutionMap.
      SubstitutionMap applySubstMap = apply->getSubstitutionMap();
      auto protoSelfToConcreteType = substitutionMap.empty()
                                         ? applySubstMap
                                         : applySubstMap.subst(substitutionMap);
      // Get a concrete protocol conformance by using the mapping for the
      // Self type of the requirement.
      auto conf = protoSelfToConcreteType.lookupConformance(
          protocol->getSelfInterfaceType()->getCanonicalType(), protocol);
      if (conf.isInvalid())
        return getUnknown(evaluator, apply,
                          UnknownReason::UnknownWitnessMethodConformance);

      callSubMap = getWitnessMethodSubstitutions(
          apply->getModule(), ApplySite(apply), callee, conf);

      /// Remark: If we ever start to care about evaluating classes,
      /// getSubstitutionsForCallee() is the analogous mapping function we
      /// should use to get correct mapping from caller to callee namespace.
      /// Ideally, the function must be renamed as
      /// getClassMethodSubstitutions().
    } else {
      callSubMap = apply->getSubstitutionMap();
    }

    // The substitution map for the callee is the composition of the callers
    // substitution map, which is always type/conformance to a concrete type
    // or conformance, with the mapping introduced by the call itself.  This
    // ensures that the callee's substitution map can map from its type
    // namespace back to concrete types and conformances.
    calleeSubMap = substitutionMap.empty() ? callSubMap
                                           : callSubMap.subst(substitutionMap);
  }

  // Now that we have successfully folded all of the parameters, we can evaluate
  // the call.
  evaluator.pushCallStack(apply->getLoc().getSourceLoc());
  SymbolicValue result;
  auto callResult = evaluateAndCacheCall(*callee, calleeSubMap, paramConstants,
                                         result, numInstEvaluated, evaluator);
  evaluator.popCallStack();

  // Return the error value the callee evaluation failed.
  if (callResult.has_value())
    return callResult.value();
  setValue(apply, result);
  return None;
}

SymbolicValue ConstExprFunctionState::getConstantValue(SILValue value) {
  // Check to see if we already have an answer.
  auto it = calculatedValues.find(value);
  if (it != calculatedValues.end())
    return it->second;

  if (!recursivelyComputeValueIfNotInState) {
    return getUnknown(evaluator, value, UnknownReason::UntrackedSILValue);
  }

  // If the client is asking for the value of a stack object that hasn't been
  // computed, and if we have to recursively compute it, the stack object must
  // be a single store value. Since this is a very different computation,
  // split it out to its own path.
  if (value->getType().isAddress() && isa<AllocStackInst>(value)) {
    return getSingleWriterAddressValue(value);
  }

  if (auto *apply = dyn_cast<ApplyInst>(value)) {
    auto callResult = computeCallResult(apply);

    // If this failed, return the error code.
    if (callResult.has_value())
      return callResult.value();

    assert(calculatedValues.count(apply));
    return calculatedValues[apply];
  }

  // Compute the value of a normal single-value instructions based on its
  // operands.
  auto result = computeConstantValue(value);

  // If this is the top-level lazy interpreter, output a debug trace.
  if (!fn) {
    LLVM_DEBUG(llvm::dbgs() << "ConstExpr top level: "; value->dump());
    LLVM_DEBUG(llvm::dbgs() << "  RESULT: "; result.dump());
  }

  setValue(value, result);
  return result;
}

/// This is a helper function for `getSingleWriterAddressValue`. Callers should
/// use `getSingleWriterAddressValue`.
///
/// If `addr` has no writing uses, returns None.
///
/// If the following conditions hold:
///   * `addr` points at uninitialized memory;
///   * there are write(s) to `addr` that, taken together, set the memory
///     exactly once (e.g. a single "store" to `addr` OR multiple "store"s to
///     different "tuple_element_addr"s of `addr`); and
///   * the writes' value(s) can be const-evaluated;
/// Then: initializes the memory at `addr` and returns None.
///
/// Otherwise, sets the memory at `addr` to an unknown SymbolicValue, and
/// returns the unknown SymbolicValue.
///
/// Additional side effects: In all cases, this function might cache address
/// values for `addr` and for addresses derived from `addr`.
///
/// Precondition: An address for `addr`, or an address that `addr` is derived
/// from, must be cached in `computedValues`.
llvm::Optional<SymbolicValue>
ConstExprFunctionState::initializeAddressFromSingleWriter(SILValue addr) {
  LLVM_DEBUG(llvm::dbgs() << "ConstExpr: initializeAddressFromSingleWriter "
             << addr);

  SmallVector<unsigned, 4> accessPath;
  auto *memoryObject = getConstantValue(addr).getAddressValue(accessPath);

  // If we detect instructions that initialize an aggregate piecewise, then we
  // set this flag, which tells us to verify that the entire aggregate has been
  // initialized.
  bool mustCheckAggregateInitialized = false;

  // Sets the pointed-at memory to `value`.
  auto setMemoryValue = [&](SymbolicValue value) {
    memoryObject->setIndexedElement(accessPath, value,
                                    evaluator.getAllocator());
  };

  // Gets the pointed-at memory value.
  auto getMemoryValue = [&]() -> SymbolicValue {
    return memoryObject->getIndexedElement(accessPath);
  };

  // Does all error-condition side-effects, and returns the appropriate error
  // result.
  // Precondition: `unknown` must be an unknown SymbolicValue.
  auto error = [&](SymbolicValue unknown) -> SymbolicValue {
    assert(unknown.getKind() == SymbolicValue::Unknown);
    setMemoryValue(unknown);
    return unknown;
  };

  // Checks that the pointed-at aggregate is fully initialized.
  // Precondition: The pointed-at memory value is uninit memory or an
  // aggregate.
  auto checkAggregateInitialized = [&]() -> bool {
    auto memoryValue = getMemoryValue();
    return memoryValue.getKind() != SymbolicValue::UninitMemory &&
           llvm::all_of(memoryValue.getAggregateMembers(),
                        [](SymbolicValue v) { return v.isConstant(); });
  };

  // Okay, check out all of the users of this value looking for semantic stores
  // into the address.  If we find more than one, then this was a var or
  // something else we can't handle.
  // We must iterate over all uses, to make sure there is a single initializer.
  // The only permitted early exit is when we know for sure that we have failed.
  for (auto *use : addr->getUses()) {
    auto user = use->getUser();

    // Ignore markers, loads, and other things that aren't stores to this stack
    // value.
    if (isa<LoadInst>(user) || isa<DeallocStackInst>(user) ||
        isa<DestroyAddrInst>(user) || DebugValueInst::hasAddrVal(user))
      continue;

    // TODO: Allow BeginAccess/EndAccess users.

    // If this is a store *to* the memory, analyze the input value.
    if (auto *si = dyn_cast<StoreInst>(user)) {
      if (use->getOperandNumber() == 1) {
        // Forbid multiple assignment.
        if (getMemoryValue().getKind() != SymbolicValue::UninitMemory)
          return error(getUnknown(evaluator, addr,
                                  UnknownReason::MutipleTopLevelWriters));

        auto result = getConstantValue(si->getOperand(0));
        if (!result.isConstant())
          return error(result);
        setMemoryValue(result);
        continue;
      }
    }

    if (auto *cai = dyn_cast<CopyAddrInst>(user)) {
      // If this is a copy_addr *from* the memory, then it is a load, ignore it.
      if (use->getOperandNumber() == 0)
        continue;

      // If this is a copy_addr *to* the memory, analyze the input value.
      assert(use->getOperandNumber() == 1 && "copy_addr has two operands");

      // Forbid multiple assignment.
      if (getMemoryValue().getKind() != SymbolicValue::UninitMemory)
        return error(
            getUnknown(evaluator, addr, UnknownReason::MutipleTopLevelWriters));

      auto result = getConstAddrAndLoadResult(cai->getOperand(0));
      if (!result.isConstant())
        return error(result);

      setMemoryValue(result);
      continue;
    }

    // If this is an apply_inst passing the memory address as an indirect
    // result operand, then we have a call that fills in this result.
    if (auto *apply = dyn_cast<ApplyInst>(user)) {
      auto conventions = apply->getSubstCalleeConv();

      // If this is an out-parameter, it is like a store.  If not, this is an
      // indirect read which is ok.
      unsigned numIndirectResults = conventions.getNumIndirectSILResults();
      unsigned opNum = use->getOperandNumber() - 1;
      if (opNum >= numIndirectResults)
        continue;

      // Forbid multiple assignment.
      if (getMemoryValue().getKind() != SymbolicValue::UninitMemory)
        return error(
            getUnknown(evaluator, addr, UnknownReason::MutipleTopLevelWriters));

      // The callee needs to be a direct call to a constant expression.
      auto callResult = computeCallResult(apply);

      // If the call failed, we're done.
      if (callResult.has_value())
        return error(*callResult);

      // computeCallResult will have figured out the result and cached it for
      // us.
      assert(getMemoryValue().isConstant());
      continue;
    }

    // If it is an index_addr, make sure it is a different address from base.
    if (auto *iai = dyn_cast<IndexAddrInst>(user)) {
      assert(use->get() == iai->getBase());
      if (auto *ili = dyn_cast<IntegerLiteralInst>(iai->getIndex())) {
        if (ili->getValue().getLimitedValue() != 0)
          continue;
      }
      return error(
          getUnknown(evaluator, addr, UnknownReason::NotTopLevelConstant));
    }

    if (auto *teai = dyn_cast<TupleElementAddrInst>(user)) {
      // Try finding a writer among the users of `teai`. For example:
      //   %179 = alloc_stack $(Int32, Int32, Int32, Int32)
      //   %183 = tuple_element_addr %179 : $*(Int32, Int32, Int32, Int32), 3
      //   copy_addr %114 to [init] %183 : $*Int32
      //   %191 = tuple_element_addr %179 : $*(Int32, Int32, Int32, Int32), 3
      //   copy_addr [take] %191 to [init] %178 : $*Int32
      //
      // The workflow is: when const-evaluating %178, we const-evaluate %191,
      // which in turn triggers const-evaluating %179, thereby enter this
      // function, where `addrInst` being %179. Among its users, %191 is not an
      // initializer, so we skip it (`initializeAddressFromSingleWriter(teai)`
      // below will act as a no-op on it). %183 is a good initializer and can
      // be const-evaluated (by const-evaluating %114).

      // We can't forbid multiple assignment here by checking for uninit memory,
      // because previous TupleElementAddrInsts may have already partially
      // initialized the memory. However, the recursive call to
      // `initializeAddressFromSingleWriter` below detects and forbids multiple
      // assignment, so we don't need to do it here.

      if (auto failure = initializeAddressFromSingleWriter(teai))
        return error(*failure);

      // If this instruction partially initialized the memory, then we must
      // remember to check later that the memory has been fully initialized.
      if (getMemoryValue().getKind() != SymbolicValue::UninitMemory)
        mustCheckAggregateInitialized = true;

#ifndef NDEBUG
      // If all aggregate elements are const, we have successfully
      // const-evaluated the entire tuple!
      if (checkAggregateInitialized())
        LLVM_DEBUG(llvm::dbgs() << "Const-evaluated the entire tuple: ";
                   getMemoryValue().dump());
#endif // NDEBUG
      continue;
    }

    LLVM_DEBUG(llvm::dbgs()
               << "Unknown SingleStore ConstExpr user: " << *user << "\n");

    // If this is some other user that we don't know about, then we should
    // treat it conservatively, because it could store into the address.
    return error(
        getUnknown(evaluator, addr, UnknownReason::NotTopLevelConstant));
  }

  if (mustCheckAggregateInitialized && !checkAggregateInitialized())
    return error(
        getUnknown(evaluator, addr, UnknownReason::NotTopLevelConstant));

  return None;
}

/// Find the initializer (single writer) of `addr` among it users,
/// const-evaluate it and store the result into a memory object.
///
/// Side effects: Creates a fully-initialized memory object (on success), or a
/// memory object containing an unknown (on failure). Inserts the address of
/// that memory object into `calculatedValues`, with key `addr`.
///
/// Returns the address of the memory object on success. Returns the unknown on
/// failure.
///
/// Some use cases are:
/// 1. When analyzing the top-level code involved in a constant expression, we
/// can end up demanding values that are returned by address.  Handle this by
/// finding the temporary stack value (an alloc_stack inst), and calling this
/// method on it.
/// 2. When const-evaluating an array via decodeAllocUninitializedArray(),
/// do that by const-evaluating the writers of individual array elements.
///
///  There are a few forms of writers, such as:
/// - store %3 to %4 ...
/// - %8 = pointer_to_address %7 : $Builtin.RawPointer to [strict] $*Int32
/// - %14 = index_addr %9 : $*Int32, %13 : $Builtin.Word
/// - %180 = tuple_element_addr %179 : $*(Int32, Int32, Int32, Int32), 3
///
///  Note unlike getConstAddrAndLoadResult(), this method does *not*
///  const-evaluate the input `addr` by evaluating its operand first, such as %7
///  above. Instead, it finds a user of %8 who is the initializer, and uses that
///  to set the const value for %7. In other words, this method propagates const
///  info from result to operand (e.g. from %8 to %7), while
///  getConstAddrAndLoadResult() propagates const info from operand to result.
///
///  As such, when const-evaluating an address-typed inst such as
///  pointer_to_address, if the address is to be written to, caller should call
///  this method (e.g. a[3] = 17). If the address is to be read (e.g. let v =
///  a[3]), call getConstAddrAndLoadResult().
SymbolicValue
ConstExprFunctionState::getSingleWriterAddressValue(SILValue addr) {
  // Check to see if we already have an answer.
  auto it = calculatedValues.find(addr);
  if (it != calculatedValues.end())
    return it->second;

  assert(addr->getType().isAddress());
  auto *addrInst = dyn_cast<SingleValueInstruction>(addr);
  if (!addrInst)
    return getUnknown(evaluator, addr, UnknownReason::NotTopLevelConstant);

  // Create a memory object to initialize, and point `addr` at it.
  auto memoryAddress =
      createMemoryObject(addr, SymbolicValue::getUninitMemory());
  auto *memoryObject = memoryAddress.getAddressValueMemoryObject();

  if (auto failure = initializeAddressFromSingleWriter(addr)) {
    assert(failure->getKind() == SymbolicValue::Unknown);
    memoryObject->setValue(*failure);
    return *failure;
  }
  if (!memoryObject->getValue().isConstant()) {
    auto unknown =
        getUnknown(evaluator, addr, UnknownReason::NotTopLevelConstant);
    memoryObject->setValue(unknown);
    return unknown;
  }

  return memoryAddress;
}

/// Given the operand to a load, resolve it to a constant if possible.
/// Also see the comments on getSingleWriterAddressValue() to contrast these 2
/// APIs.
SymbolicValue ConstExprFunctionState::getConstAddrAndLoadResult(SILValue addr) {
  auto addrVal = getConstantValue(addr);
  if (!addrVal.isConstant())
    return addrVal;

  return loadAddrValue(addr, addrVal);
}

/// Load and return the underlying (const) object whose address is given by
/// `addrVal`. On error, return a message based on `addr`.
SymbolicValue ConstExprFunctionState::loadAddrValue(SILValue addr,
                                                    SymbolicValue addrVal) {
  SmallVector<unsigned, 4> accessPath;
  auto *memoryObject = addrVal.getAddressValue(accessPath);

  // If this is a derived address, then we are digging into an aggregate
  // value.
  auto objectVal = memoryObject->getValue();

  // Try digging through the aggregate to get to our value.
  unsigned idx = 0, end = accessPath.size();
  while (idx != end && objectVal.getKind() == SymbolicValue::Aggregate) {
    objectVal = objectVal.getAggregateMembers()[accessPath[idx]];
    ++idx;
  }

  // If we successfully indexed down to our value, then we're done.
  if (idx == end)
    return objectVal;

  // If the memory object had a reason, return it.
  if (objectVal.isUnknown())
    return objectVal;

  // Otherwise, return a generic failure.
  return getUnknown(evaluator, addr, UnknownReason::InvalidOperandValue);
}

/// Evaluate a flow sensitive store to the specified pointer address.
llvm::Optional<SymbolicValue>
ConstExprFunctionState::computeFSStore(SymbolicValue storedCst, SILValue dest) {
  // Only update existing memory locations that we're tracking.
  auto it = calculatedValues.find(dest);
  if (it == calculatedValues.end())
    return getUnknown(evaluator, dest, UnknownReason::UntrackedSILValue);
  if (!it->second.isConstant())
    return getUnknown(evaluator, dest, UnknownReason::InvalidOperandValue);

  SmallVector<unsigned, 4> accessPath;
  auto *memoryObject = it->second.getAddressValue(accessPath);
  memoryObject->setIndexedElement(accessPath, storedCst,
                                  evaluator.getAllocator());
  return None;
}

llvm::Optional<SymbolicValue> ConstExprFunctionState::evaluateClosureCreation(
    SingleValueInstruction *closureInst) {
  assert(isa<PartialApplyInst>(closureInst) ||
         isa<ThinToThickFunctionInst>(closureInst));
  SILValue calleeOperand = closureInst->getOperand(0);
  SymbolicValue calleeValue = getConstantValue(calleeOperand);
  if (!calleeValue.isConstant())
    return calleeValue;
  if (calleeValue.getKind() != SymbolicValue::Function) {
    return getUnknown(evaluator, closureInst,
                      UnknownReason::InvalidOperandValue);
  }

  SILFunction *target = calleeValue.getFunctionValue();
  assert(target != nullptr);

  SmallVector<SymbolicClosureArgument, 4> captures;

  // Map generic parameters of the target to the generic arguments passed to the
  // call.
  SubstitutionMap callSubstMap;

  // If this is a partial-apply instruction, arguments to this partial-apply
  // instruction are the captures of the closure.
  if (PartialApplyInst *papply = dyn_cast<PartialApplyInst>(closureInst)) {
    for (SILValue capturedSILValue : papply->getArguments()) {
      SymbolicValue capturedSymbolicValue = getConstantValue(capturedSILValue);
      if (!capturedSymbolicValue.isConstant()) {
        captures.push_back({capturedSILValue, None});
        continue;
      }
      captures.push_back({capturedSILValue, capturedSymbolicValue});
    }
    SubstitutionMap applySubstMap = papply->getSubstitutionMap();
    callSubstMap = substitutionMap.empty()
                       ? applySubstMap
                       : applySubstMap.subst(substitutionMap);
  }

  auto closureVal = SymbolicValue::makeClosure(
      target, captures, callSubstMap, closureInst, evaluator.getAllocator());
  setValue(closureInst, closureVal);
  return None;
}

/// Evaluate the specified instruction in a flow sensitive way, for use by
/// the constexpr function evaluator.  This does not handle control flow
/// statements.  This returns None on success, and an Unknown SymbolicValue with
/// information about an error on failure.
llvm::Optional<SymbolicValue>
ConstExprFunctionState::evaluateFlowSensitive(SILInstruction *inst) {
  // These are just markers.
  if (isa<DebugValueInst>(inst) || isa<EndAccessInst>(inst) ||
      // The interpreter doesn't model these memory management instructions, so
      // skip them.
      isa<DestroyAddrInst>(inst) || isa<RetainValueInst>(inst) ||
      isa<ReleaseValueInst>(inst) || isa<StrongRetainInst>(inst) ||
      isa<StrongReleaseInst>(inst) || isa<DestroyValueInst>(inst) ||
      isa<EndBorrowInst>(inst) || isa<DebugStepInst>(inst) ||
      // Skip instrumentation
      isInstrumentation(inst))
    return None;

  // If this is a special flow-sensitive instruction like a stack allocation,
  // store, copy_addr, etc, we handle it specially here.
  if (auto asi = dyn_cast<AllocStackInst>(inst)) {
    // If a struct with no stored properties is created, no initialization is
    // needed. Hence, create a empty aggregate as the initial value.
    CanType structType = asi->getElementType().getASTType();
    StructDecl *structDecl = structType.getStructOrBoundGenericStruct();

    if (structDecl && structDecl->getStoredProperties().empty()) {
      createMemoryObject(asi, SymbolicValue::getAggregate(
                                  ArrayRef<SymbolicValue>(),
                                  substituteGenericParamsAndSimplify(structType),
                                  evaluator.getAllocator()));
      return None;
    }
    createMemoryObject(asi, SymbolicValue::getUninitMemory());
    return None;
  }

  // If this is a deallocation of a memory object that we are tracking, then
  // don't do anything.  The memory is allocated in a BumpPtrAllocator so there
  // is no useful way to free it.
  if (isa<DeallocStackInst>(inst))
    return None;

  if (CondFailInst *condFail = dyn_cast<CondFailInst>(inst)) {
    auto failed = getConstantValue(inst->getOperand(0));
    if (failed.getKind() == SymbolicValue::Integer) {
      if (failed.getIntegerValue() == 0)
        return None;
      // Conditional fail actually failed.
      return evaluator.getUnknown(
          inst->asSILNode(),
          UnknownReason::createTrap(
              (Twine("trap: ") + condFail->getMessage()).str(),
              evaluator.getAllocator()));
    }
  }

  // If this is a call, evaluate it. Calls are handled separately from other
  // single-valued instructions because calls which return void will not be
  // mapped to a symbolic value. Every other single-valued instruction will be
  // mapped to a symbolic value if its evaluation is successful.
  if (auto apply = dyn_cast<ApplyInst>(inst))
    return computeCallResult(apply);

  if (isa<StoreInst>(inst) || isa<StoreBorrowInst>(inst)) {
    auto stored = getConstantValue(inst->getOperand(0));
    if (!stored.isConstant())
      return stored;

    return computeFSStore(stored, inst->getOperand(1));
  }

  // Copy addr is a load + store combination.
  if (auto *copy = dyn_cast<CopyAddrInst>(inst)) {
    auto value = getConstAddrAndLoadResult(copy->getOperand(0));
    if (!value.isConstant())
      return value;

    return computeFSStore(value, copy->getOperand(1));
  }

  if (auto *injectEnumInst = dyn_cast<InjectEnumAddrInst>(inst)) {
    return computeFSStore(SymbolicValue::getEnum(injectEnumInst->getElement()),
                          injectEnumInst->getOperand());
  }

  if (isa<PartialApplyInst>(inst) || isa<ThinToThickFunctionInst>(inst)) {
    return evaluateClosureCreation(cast<SingleValueInstruction>(inst));
  }

  // If the instruction produces a result, try computing it, and fail if the
  // computation fails.
  if (auto *singleValueInst = dyn_cast<SingleValueInstruction>(inst)) {
    auto result = computeConstantValue(singleValueInst);
    if (!result.isConstant())
      return result;
    setValue(singleValueInst, result);
    LLVM_DEBUG(llvm::dbgs() << "  RESULT: "; result.dump());
    return None;
  }

  if (isa<DestructureTupleInst>(inst) || isa<DestructureStructInst>(inst)) {
    auto *mvi = cast<MultipleValueInstruction>(inst);
    SymbolicValue aggVal = getConstantValue(mvi->getOperand(0));
    if (!aggVal.isConstant()) {
      return aggVal;
    }
    assert(aggVal.getKind() == SymbolicValue::Aggregate);

    ArrayRef<SymbolicValue> aggElems = aggVal.getAggregateMembers();
    assert(aggElems.size() == mvi->getNumResults());

    for (unsigned i = 0; i < mvi->getNumResults(); ++i) {
      setValue(mvi->getResult(i), aggElems[i]);
    }
    return None;
  }

  LLVM_DEBUG(llvm::dbgs() << "ConstExpr Unknown FS: " << *inst << "\n");
  // If this is an unknown instruction with no results then bail out.
  return getUnknown(evaluator, inst->asSILNode(),
                    UnknownReason::UnsupportedInstruction);
}

std::pair<Optional<SILBasicBlock::iterator>, Optional<SymbolicValue>>
ConstExprFunctionState::evaluateInstructionAndGetNext(
    SILBasicBlock::iterator instI,
    SmallPtrSetImpl<SILBasicBlock *> &visitedBlocks) {

  SILInstruction *inst = &*instI;
  // If we can evaluate this flow sensitively, then return the next instruction.
  if (!isa<TermInst>(inst)) {
    auto fsResult = evaluateFlowSensitive(inst);
    if (fsResult.has_value())
      return {None, fsResult};
    return {++instI, None};
  }

  // If this is a branch instruction, evaluate and return the target basic block.
  if (auto *br = dyn_cast<BranchInst>(inst)) {
    auto destBB = br->getDestBB();

    // If we've already visited this block then fail - we have a loop.
    if (!visitedBlocks.insert(destBB).second)
      return {None, getUnknown(evaluator, br, UnknownReason::Loop)};

    // Set up basic block arguments.
    for (unsigned i = 0, e = br->getNumArgs(); i != e; ++i) {
      auto argument = getConstantValue(br->getArg(i));
      if (!argument.isConstant())
        return {None, argument};
      setValue(destBB->getArgument(i), argument);
    }
    // Set the instruction pointer to the first instruction of the block.
    return {destBB->begin(), None};
  }

  if (auto *cbr = dyn_cast<CondBranchInst>(inst)) {
    auto val = getConstantValue(inst->getOperand(0));
    if (!val.isConstant())
      return {None, val};

    SILBasicBlock *destBB;
    if (!val.getIntegerValue())
      destBB = cbr->getFalseBB();
    else
      destBB = cbr->getTrueBB();

    // If we've already visited this block then fail - we have a loop.
    if (!visitedBlocks.insert(destBB).second)
      return {None, getUnknown(evaluator, cbr, UnknownReason::Loop)};

    return {destBB->begin(), None};
  }

  if (auto switchInst = SwitchEnumTermInst(inst)) {
    SymbolicValue value;
    if (isa<SwitchEnumInst>(*switchInst)) {
      value = getConstantValue(switchInst.getOperand());
    } else {
      value = getConstAddrAndLoadResult(switchInst.getOperand());
    }
    if (!value.isConstant())
      return {None, value};

    assert(value.getKind() == SymbolicValue::Enum ||
           value.getKind() == SymbolicValue::EnumWithPayload);

    SILBasicBlock *caseBB = switchInst.getCaseDestination(value.getEnumValue());
    if (caseBB->getNumArguments() == 0)
      return {caseBB->begin(), None};

    // Set up the arguments.

    // When there are multiple payload components, they form a single
    // tuple-typed argument.
    assert(caseBB->getNumArguments() == 1);

    if (caseBB == switchInst.getDefaultBBOrNull().getPtrOrNull()) {
      if (!switchInst.getUniqueCaseForDefault()) {
        // In OSSA, the default block forward the original enum value whenever
        // it does not correspond to a unique case.
        setValue(caseBB->getArgument(0), value);
        return {caseBB->begin(), None};
      }
    }

    assert(value.getKind() == SymbolicValue::EnumWithPayload);
    auto argument = value.getEnumPayloadValue();
    assert(argument.isConstant());
    setValue(caseBB->getArgument(0), argument);

    return {caseBB->begin(), None};
  }

  if (isa<CheckedCastBranchInst>(inst)) {
    CheckedCastBranchInst *checkedCastInst =
        dyn_cast<CheckedCastBranchInst>(inst);
    SymbolicValue value = getConstantValue(checkedCastInst->getOperand());
    if (!value.isConstant())
      return {None, value};

    // Determine success or failure of this cast.
    CanType sourceType;
    if (value.getKind() == SymbolicValue::Array) {
      sourceType = value.getArrayType()->getCanonicalType();
    } else {
      // Here, the source type cannot be an address-only type as this is
      // not a CheckedCastBranchAddr inst. Therefore, it has to be a struct
      // type or String or Metatype. Since the types of aggregates are not
      // tracked, we recover it from the declared type of the source operand
      // and generic parameter substitutions in the interpreter state.
      sourceType = substituteGenericParamsAndSimplify(
                                      checkedCastInst->getSourceFormalType());
    }
    CanType targetType = substituteGenericParamsAndSimplify(
        checkedCastInst->getTargetFormalType());
    DynamicCastFeasibility castResult = classifyDynamicCast(
        inst->getModule().getSwiftModule(), sourceType, targetType);
    if (castResult == DynamicCastFeasibility::MaySucceed) {
      return {None, getUnknown(evaluator, inst->asSILNode(),
                               UnknownReason::UnknownCastResult)};
    }
    // Determine the basic block to jump to.
    SILBasicBlock *resultBB =
        (castResult == DynamicCastFeasibility::WillSucceed)
            ? checkedCastInst->getSuccessBB()
            : checkedCastInst->getFailureBB();
    // Set up the arguments of the basic block, if any.
    if (resultBB->getNumArguments() == 0)
      return {resultBB->begin(), None};
    // There should be at most one argument to the basic block, which is the
    // casted value with the right type, or the input value if the cast fails,
    // and inst is in OSSA.
    assert(resultBB->getNumArguments() == 1);
    setValue(resultBB->getArgument(0), value);
    return {resultBB->begin(), None};
  }

  LLVM_DEBUG(llvm::dbgs() << "ConstExpr: Unknown Branch Instruction: " << *inst
                          << "\n");

  return {None, getUnknown(evaluator, inst->asSILNode(),
                           UnknownReason::UnsupportedInstruction)};
}

/// Evaluate a call to the specified function as if it were a constant
/// expression, returning None and filling in `results` on success, or
/// returning an 'Unknown' SymbolicValue on failure carrying the error.
///
static llvm::Optional<SymbolicValue>
evaluateAndCacheCall(SILFunction &fn, SubstitutionMap substitutionMap,
                     ArrayRef<SymbolicValue> arguments, SymbolicValue &result,
                     unsigned &numInstEvaluated,
                     ConstExprEvaluator &evaluator) {
  assert(!fn.isExternalDeclaration() && "Can't analyze bodyless function");
  ConstExprFunctionState state(evaluator, &fn, substitutionMap,
                               numInstEvaluated,
                               /*TopLevelEvaluation*/ false);

  // TODO: implement caching.
  // TODO: reject code that is too complex.

  unsigned nextBBArg = 0;
  const auto &argList = fn.front().getArguments();

  LLVM_DEBUG(llvm::dbgs().changeColor(raw_ostream::SAVEDCOLOR, /*bold*/ true)
                 << "\nConstExpr call fn: "
                 << Demangle::demangleSymbolAsString(fn.getName());
             llvm::dbgs().resetColor() << "\n");

  assert(arguments.size() == argList.size() && "incorrect # arguments passed");
  for (auto argSymVal : arguments)
    state.setValue(argList[nextBBArg++], argSymVal);

  // Keep track of which blocks we've already visited.  We don't support loops
  // and this allows us to reject them.
  SmallPtrSet<SILBasicBlock *, 8> visitedBlocks;

  // Keep track of the current "instruction pointer".
  SILBasicBlock::iterator nextInst = fn.front().begin();
  visitedBlocks.insert(&fn.front());

  while (1) {
    SILInstruction *inst = &*nextInst;
    LLVM_DEBUG(llvm::dbgs() << "ConstExpr interpret: "; inst->dump());

    // Make sure we haven't exceeded our interpreter iteration cap.
    if (++numInstEvaluated > ConstExprLimit) {
      return getUnknown(evaluator, inst->asSILNode(),
                        UnknownReason::TooManyInstructions);
    }

    if (isa<ReturnInst>(inst)) {
      auto val = state.getConstantValue(inst->getOperand(0));
      if (!val.isConstant())
        return val;

      // If we got a constant value, then we're good. Set up the normal result
      // values as well as any indirect results.
      result = val;

      // TODO: Handle caching of results.

      LLVM_DEBUG(llvm::dbgs() << "\n");
      return None;
    }

    // Handle other instructions here.
    Optional<SILBasicBlock::iterator> nextInstOpt = None;
    Optional<SymbolicValue> errorVal = None;

    std::tie(nextInstOpt, errorVal) =
        state.evaluateInstructionAndGetNext(nextInst, visitedBlocks);

    if (errorVal.has_value())
      return errorVal;

    assert(nextInstOpt.has_value());
    nextInst = nextInstOpt.value();
  }
}

//===----------------------------------------------------------------------===//
// ConstExprEvaluator implementation.
//===----------------------------------------------------------------------===//

ConstExprEvaluator::ConstExprEvaluator(SymbolicValueAllocator &alloc,
                                       unsigned assertConf, bool trackCallees)
    : allocator(alloc), assertConfig(assertConf), trackCallees(trackCallees) {}

ConstExprEvaluator::~ConstExprEvaluator() {}

/// An explicit copy constructor.
ConstExprEvaluator::ConstExprEvaluator(const ConstExprEvaluator &other)
    : allocator(other.allocator) {
  callStack = other.callStack;
}

SymbolicValue ConstExprEvaluator::getUnknown(SILNode *node,
                                             UnknownReason reason) {
  return SymbolicValue::getUnknown(node, reason, getCallStack(),
                                   getAllocator());
}

/// Analyze the specified values to determine if they are constant values. This
/// is done in code that is not necessarily itself a constexpr function. The
/// results are added to the results list which is a parallel structure to the
/// input values.
void ConstExprEvaluator::computeConstantValues(
    ArrayRef<SILValue> values, SmallVectorImpl<SymbolicValue> &results) {
  unsigned numInstEvaluated = 0;
  ConstExprFunctionState state(*this, /*SILFunction*/ nullptr, {},
                               numInstEvaluated,
                               /*enableTopLevelEvaluation*/ true);
  for (auto v : values) {
    auto symVal = state.getConstantValue(v);
    results.push_back(symVal);

    // Reset the execution limit back to zero for each subexpression we look
    // at.  We don't want lots of constants folded to trigger a limit.
    numInstEvaluated = 0;
  }
}

//===----------------------------------------------------------------------===//
// ConstExprStepEvaluator implementation.
//===----------------------------------------------------------------------===//

ConstExprStepEvaluator::ConstExprStepEvaluator(SymbolicValueAllocator &alloc,
                                               SILFunction *fun,
                                               unsigned assertConf,
                                               bool trackCallees)
    : evaluator(alloc, assertConf, trackCallees),
      internalState(
          new ConstExprFunctionState(evaluator, fun, {}, stepsEvaluated,
                                     /*enableTopLevelEvaluation*/ false)) {
  assert(fun);
}

ConstExprStepEvaluator::~ConstExprStepEvaluator() { delete internalState; }

std::pair<Optional<SILBasicBlock::iterator>, Optional<SymbolicValue>>
ConstExprStepEvaluator::evaluate(SILBasicBlock::iterator instI) {
  // Reset `stepsEvaluated` to zero.
  stepsEvaluated = 0;
  return internalState->evaluateInstructionAndGetNext(instI, visitedBlocks);
}

void ConstExprStepEvaluator::setMutableAddressesToUnknown(
    SILInstruction *inst) {
  for (auto &operand : inst->getAllOperands()) {
    auto constValOpt = lookupConstValue(operand.get());
    if (!constValOpt) {
      continue;
    }
    auto constVal = constValOpt.value();
    auto constKind = constVal.getKind();

    // Skip can only be invoked on value types or addresses of value types.
    // Note that adding a new kind of symbolic value may require handling its
    // side-effects, especially if that symbolic value does not represent a
    // value type.
    assert(constKind == SymbolicValue::Address ||
           constKind == SymbolicValue::Unknown ||
           constKind == SymbolicValue::Metatype ||
           constKind == SymbolicValue::Function ||
           constKind == SymbolicValue::Integer ||
           constKind == SymbolicValue::String ||
           constKind == SymbolicValue::Aggregate ||
           constKind == SymbolicValue::Enum ||
           constKind == SymbolicValue::EnumWithPayload ||
           constKind == SymbolicValue::Array ||
           constKind == SymbolicValue::Closure ||
           constKind == SymbolicValue::UninitMemory);

    if (constKind != SymbolicValue::Address) {
      continue;
    }

    // If the address is only used @in_guaranteed or @in_constant, there
    // can be no mutation through this address. Therefore, ignore it.
    if (ApplyInst *applyInst = dyn_cast<ApplyInst>(inst)) {
      ApplySite applySite(applyInst);
      SILArgumentConvention convention =
        applySite.getArgumentConvention(operand);
      if (convention == SILArgumentConvention::Indirect_In_Guaranteed) {
        continue;
      }
    }

    // Write an unknown value into the address.
    SmallVector<unsigned, 4> accessPath;
    auto *memoryObject = constVal.getAddressValue(accessPath);
    auto unknownValue = SymbolicValue::getUnknown(
        inst->asSILNode(),
        UnknownReason::create(UnknownReason::MutatedByUnevaluatedInstruction),
        {}, evaluator.getAllocator());

    auto memoryContent = memoryObject->getValue();
    if (memoryContent.getKind() == SymbolicValue::Aggregate) {
      memoryObject->setIndexedElement(accessPath, unknownValue,
                                      evaluator.getAllocator());
    } else {
      memoryObject->setValue(unknownValue);
    }
  }
}

std::pair<Optional<SILBasicBlock::iterator>, Optional<SymbolicValue>>
ConstExprStepEvaluator::skipByMakingEffectsNonConstant(
    SILBasicBlock::iterator instI) {
  SILInstruction *inst = &(*instI);

  if (inst->mayWriteToMemory())
    setMutableAddressesToUnknown(inst);

  // Map the results of this instruction to unknown values.
  for (auto result : inst->getResults()) {
    internalState->setValue(
        result, SymbolicValue::getUnknown(
                    inst->asSILNode(),
                    UnknownReason::create(
                        UnknownReason::ReturnedByUnevaluatedInstruction),
                    {}, evaluator.getAllocator()));
  }

  // If we have a next instruction in the basic block return it.
  // Otherwise, return None for the next instruction.
  // Note that we can find the next instruction in the case of unconditional
  // branches. But, there is no real need to do that as of now.
  if (!isa<TermInst>(inst)) {
    return {++instI, None};
  }
  return {None, None};
}

bool swift::isFailStopError(SymbolicValue errorVal) {
  assert(errorVal.isUnknown());

  switch (errorVal.getUnknownReason().getKind()) {
  case UnknownReason::TooManyInstructions:
  case UnknownReason::Overflow:
  case UnknownReason::Trap:
    return true;
  default:
    return false;
  }
}

std::pair<Optional<SILBasicBlock::iterator>, Optional<SymbolicValue>>
ConstExprStepEvaluator::tryEvaluateOrElseMakeEffectsNonConstant(
    SILBasicBlock::iterator instI) {
  auto evaluateResult = evaluate(instI);
  Optional<SILBasicBlock::iterator> nextI = evaluateResult.first;
  Optional<SymbolicValue> errorVal = evaluateResult.second;

  if (!errorVal) {
    assert(nextI);
    return evaluateResult;
  }
  assert(!nextI);

  if (isFailStopError(*errorVal)) {
    return evaluateResult;
  }

  // If evaluation fails on an unconditional branch, it implies there is a loop
  // at the top level.
  if (isa<BranchInst>(&(*instI))) {
    assert(errorVal->getUnknownReason().getKind() == UnknownReason::Loop);
    return evaluateResult;
  }

  // Since the evaluation has failed, make the effects of this instruction
  // unknown.
  auto result = skipByMakingEffectsNonConstant(instI);
  return {result.first, errorVal};
}

Optional<SymbolicValue>
ConstExprStepEvaluator::lookupConstValue(SILValue value) {
  auto res = internalState->lookupValue(value);
  if (res && !res->isConstant()) {
    return None;
  }
  return res;
}

void ConstExprStepEvaluator::dumpState() { internalState->dump(); }

bool swift::isKnownConstantEvaluableFunction(SILFunction *fun) {
  return classifyFunction(fun).has_value();
}

bool swift::hasConstantEvaluableAnnotation(SILFunction *fun) {
  assert(fun && "fun should not be nullptr");
  return fun->hasSemanticsAttr("constant_evaluable");
}

bool swift::isConstantEvaluable(SILFunction *fun) {
  return hasConstantEvaluableAnnotation(fun) ||
         isKnownConstantEvaluableFunction(fun);
}

/// Return true iff the \p applySite is constant-evaluable and read-only.
///
/// Functions annotated as "constant_evaluable" are assumed to be "side-effect
/// free", unless their signature and substitution map indicates otherwise. A
/// constant_evaluable function call is read only unless it:
///   (1) has generic parameters
///   (2) has inout parameters
///   (3) has indirect results
///
/// Read-only constant evaluable functions can do only the following and
/// nothing else:
///   (1) The call may read any memory location.
///   (2) The call may destroy owned parameters i.e., consume them.
///   (3) The call may write into memory locations newly created by the call.
///   (4) The call may use assertions, which traps at runtime on failure.
///   (5) The call may return a non-generic value.
///
/// Essentially, these are calls whose "effect" is visible only in their return
/// value or through the parameters that are destroyed. The return value
/// is also guaranteed to have value semantics as it is non-generic and
/// reference semantics is not constant evaluable.
bool swift::isReadOnlyConstantEvaluableCall(FullApplySite applySite) {
  SILFunction *callee = applySite.getCalleeFunction();
  if (!callee)
    return false;

  if (auto knownFunction = classifyFunction(callee)) {
    return isReadOnlyFunction(knownFunction.value());
  }
  if (!hasConstantEvaluableAnnotation(callee))
    return false;

  return !applySite.hasSubstitutions() && !getNumInOutArguments(applySite)
         && !applySite.getNumIndirectSILResults();
}
