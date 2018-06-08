//===--- TFConstExpr.cpp - TensorFlow constant expressions ----------------===//
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

#define DEBUG_TYPE "TFConstExpr"
#include "TFConstExpr.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/Basic/Defer.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;
using namespace tf;

static llvm::cl::opt<unsigned>
ConstExprLimit("constexpr-limit", llvm::cl::init(256),
               llvm::cl::desc("Number of instructions interpreted in a"
                              " constexpr function"));

static llvm::Optional<SymbolicValue>
evaluateAndCacheCall(SILFunction &fn, SubstitutionList substitutions,
                     ArrayRef<SymbolicValue> arguments,
                     SmallVectorImpl<SymbolicValue> &results,
                     unsigned &numInstEvaluated,
                     ConstExprEvaluator &evaluator);



//===----------------------------------------------------------------------===//
// ConstExprFunctionCache implementation.
//===----------------------------------------------------------------------===//

namespace {
  /// This type represents a cache of computed values within a specific function
  /// as evaluation happens.  A separate instance of this is made for each
  /// callee in a call chain to represent the constant values given the set of
  /// formal parameters that callee was invoked with.
  class ConstExprFunctionCache {
    /// This is the evaluator we put bump pointer allocated values into.
    ConstExprEvaluator &evaluator;

    /// If we are analyzing the body of a constexpr function, this is the
    /// function.  This is null for the top-level expression.
    SILFunction *fn;

    /// If we have a function being analyzed, this is the substitution list for
    /// the call to it.
    SubstitutionList substitutions;

    /// This is a mapping of substitutions.
    SubstitutionMap substitutionMap;

    /// This keeps track of the number of instructions we've evaluated.  If this
    /// goes beyond the execution cap, then we start returning unknown values.
    unsigned &numInstEvaluated;

    /// This is a cache of previously analyzed values, maintained and filled in
    /// by getConstantValue.
    llvm::DenseMap<SILValue, SymbolicValue> calculatedValues;

  public:
    ConstExprFunctionCache(ConstExprEvaluator &evaluator, SILFunction *fn,
                           SubstitutionList substitutions,
                           unsigned &numInstEvaluated)
      : evaluator(evaluator), fn(fn), substitutions(substitutions),
        numInstEvaluated(numInstEvaluated) {

      if (fn && !substitutions.empty()) {
        auto signature = fn->getLoweredFunctionType()->getGenericSignature();
        if (signature)
          substitutionMap = signature->getSubstitutionMap(substitutions);
      }
    }

    void setValue(SILValue value, SymbolicValue symVal) {
      calculatedValues.insert({ value, symVal });
    }

    /// Return the Symbolic value for the specified SIL value.
    SymbolicValue getConstantValue(SILValue value);


    /// Evaluate the specified instruction in a flow sensitive way, for use by
    /// the constexpr function evaluator.  This does not handle control flow
    /// statements.
    llvm::Optional<SymbolicValue> evaluateFlowSensitive(SILInstruction *inst);
  private:
    Type simplifyType(Type ty);
    SymbolicValue computeConstantValue(SILValue value);
    SymbolicValue computeConstantValueBuiltin(BuiltinInst *inst);

    SymbolicValue computeSingleStoreAddressValue(SILValue addr);
    llvm::Optional<SymbolicValue> computeCallResult(ApplyInst *apply);
  };
} // end anonymous namespace


/// Simplify the specified type based on knowledge of substitutions if we have
/// any.
Type ConstExprFunctionCache::simplifyType(Type ty) {
  return substitutionMap.empty() ? ty : ty.subst(substitutionMap);
}

/// Lazily initialize the specified SIL Loader.
static SerializedSILLoader &
initLoader(std::unique_ptr<SerializedSILLoader> &silLoader, SILModule &module) {
  if (!silLoader)
    silLoader = SerializedSILLoader::create(module.getASTContext(),
                                            &module, nullptr);
  return *silLoader;
}


// TODO: refactor this out somewhere sharable between autodiff and this code.
static SILWitnessTable *
lookupOrLinkWitnessTable(ProtocolConformanceRef confRef, SILModule &module,
                         std::unique_ptr<SerializedSILLoader> &silLoader) {
  auto *conf = confRef.getConcrete();
  auto wtable = module.lookUpWitnessTable(conf);
  if (wtable) return wtable;



  auto *decl =
    conf->getDeclContext()->getAsNominalTypeOrNominalTypeExtensionContext();
  auto linkage = getSILLinkage(getDeclLinkage(decl), NotForDefinition);
  auto *newTable = module.createWitnessTableDeclaration(conf, linkage);
  newTable = initLoader(silLoader, module).lookupWitnessTable(newTable);
  // Update linkage for witness methods.
  // FIXME: Figure out why witnesses have shared linkage by default.
  for (auto &entry : newTable->getEntries())
    if (entry.getKind() == SILWitnessTable::WitnessKind::Method)
      entry.getMethodWitness().Witness->setLinkage(linkage);
  return newTable;
}

SymbolicValue ConstExprFunctionCache::computeConstantValue(SILValue value) {
  // If this a trivial constant instruction that we can handle, then fold it
  // immediately.
  if (isa<IntegerLiteralInst>(value) || isa<FloatLiteralInst>(value) ||
      isa<StringLiteralInst>(value))
    return SymbolicValue::getConstantInst(cast<SingleValueInstruction>(value));

  if (auto *fri = dyn_cast<FunctionRefInst>(value))
    return SymbolicValue::getFunction(fri->getReferencedFunction());

  // If we have a reference to a metatype, constant fold any substitutable
  // types.
  if (auto *mti = dyn_cast<MetatypeInst>(value)) {
    auto metatype = mti->getType().castTo<MetatypeType>();
    auto type = simplifyType(metatype->getInstanceType())->getCanonicalType();
    return SymbolicValue::getMetatype(type);
  }

  if (auto *tei = dyn_cast<TupleExtractInst>(value)) {
    auto val = getConstantValue(tei->getOperand());
    if (!val.isConstant()) return val;
    return val.getAggregateValue()[tei->getFieldNo()];
  }

  // If this is a struct extract from a fragile type, then we can return the
  // element being extracted.
  if (auto *sei = dyn_cast<StructExtractInst>(value)) {
    auto val = getConstantValue(sei->getOperand());
    if (!val.isConstant()) return val;
    return val.getAggregateValue()[sei->getFieldNo()];
  }

  // TODO: If this is a single element struct, we can avoid creating an
  // aggregate to reduce # allocations.  This is extra silly in the case of zero
  // element tuples.
  if (isa<StructInst>(value) || isa<TupleInst>(value)) {
    auto inst = cast<SingleValueInstruction>(value);
    SmallVector<SymbolicValue, 4> elts;

    for (unsigned i = 0, e = inst->getNumOperands(); i != e; ++i) {
      auto val = getConstantValue(inst->getOperand(i));
      if (!val.isConstant()) return val;
      elts.push_back(val);
    }

    return SymbolicValue::getAggregate(elts, evaluator.getAllocator());
  }

  // If this is a struct or tuple element addressor, compute a more derived
  // address.
  if (isa<StructElementAddrInst>(value) || isa<TupleElementAddrInst>(value)) {
    unsigned index;
    if (auto sea = dyn_cast<StructElementAddrInst>(value))
      index = sea->getFieldNo();
    else
      index = cast<TupleElementAddrInst>(value)->getFieldNo();

    auto inst = cast<SingleValueInstruction>(value);
    SILValue base = inst->getOperand(0);
    auto baseVal = getConstantValue(base);
    SmallVector<unsigned, 4> indices;
    // If the base is an address object, then this is adding indices onto the
    // list.  Otherwise, this is the first reference to some memory value.
    if (baseVal.isAddress()) {
      auto baseIndices = baseVal.getAddressIndices();
      base = baseVal.getAddressBase();
      indices.append(baseIndices.begin(), baseIndices.end());
    }
    indices.push_back(index);
    return SymbolicValue::getAddress(base, indices, evaluator.getAllocator());
  }

  // If this is a load, then we either have computed the value of the memory
  // already (when analyzing the body of a constexpr) or this should be a by-ref
  // result of a call.  Either way, we ask for the value of the pointer: in the
  // former case this will be the latest value for this, in the later case, this
  // must be a single-def value for us to analyze it.
  if (auto li = dyn_cast<LoadInst>(value)) {
    auto result = getConstantValue(li->getOperand());
    // If it is some non-address value, then this is a direct reference to
    // memory.
    if (result.isConstant() && !result.isAddress())
      return result;

    // If this is a derived address, then we are digging into an aggregate
    // value.
    if (result.isAddress()) {
      auto baseVal = getConstantValue(result.getAddressBase());
      auto indices = result.getAddressIndices();
      // Try digging through the aggregate to get to our value.
      while (!indices.empty() &&
             baseVal.getKind() == SymbolicValue::Aggregate) {
        baseVal = baseVal.getAggregateValue()[indices.front()];
        indices = indices.drop_front();
      }

      // If we successfully indexed down to our value, then we're done.
      if (indices.empty())
        return baseVal;
    }

    // When accessing a var in top level code, we want to report the error at
    // the site of the load, not the site of the memory definition.  Remap an
    // unknown result to be the load if present.
    return SymbolicValue::getUnknown(value, UnknownReason::Default);
  }

  // Try to resolve a witness method against our known conformances.
  if (auto *wmi = dyn_cast<WitnessMethodInst>(value)) {
    auto confResult = substitutionMap.lookupConformance(wmi->getLookupType(),
                         wmi->getConformance().getRequirement());
    if (!confResult)
      return SymbolicValue::getUnknown(value, UnknownReason::Default);
    auto conf = confResult.getValue();
    auto &module = wmi->getModule();

    // Look up the conformance's withness table and the member out of it.
    SILFunction *fn =
      module.lookUpFunctionInWitnessTable(conf, wmi->getMember()).first;
    if (!fn) {
      // If that failed, try force loading it, and try again.
      (void)lookupOrLinkWitnessTable(conf, wmi->getModule(),
                                     evaluator.getSILLoader());
      fn = module.lookUpFunctionInWitnessTable(conf, wmi->getMember()).first;
    }

    // If we were able to resolve it, then we can proceed.
    if (fn)
      return SymbolicValue::getFunction(fn);
  }

  if (auto *builtin = dyn_cast<BuiltinInst>(value))
    return computeConstantValueBuiltin(builtin);

  if (auto *apply = dyn_cast<ApplyInst>(value)) {
    auto callResult = computeCallResult(apply);

    // If this failed, return the error code.
    if (callResult.hasValue())
      return callResult.getValue();

    assert(calculatedValues.count(apply));
    return calculatedValues[apply];
  }


  DEBUG(llvm::errs() << "ConstExpr Unknown simple: " << *value << "\n");

  // Otherwise, we don't know how to handle this.
  return SymbolicValue::getUnknown(value, UnknownReason::Default);
}

SymbolicValue
ConstExprFunctionCache::computeConstantValueBuiltin(BuiltinInst *inst) {
  const BuiltinInfo &builtin = inst->getBuiltinInfo();

  // Handle various cases in groups.

  // Unary operations first.
  if (inst->getNumOperands() == 1) {
    auto operand = getConstantValue(inst->getOperand(0));
    // TODO: Could add a "value used here" sort of diagnostic.
    if (!operand.isConstant())
      return operand;

    // TODO: SUCheckedConversion/USCheckedConversion

    // Implement support for s_to_s_checked_trunc_Int2048_Int64 and other
    // checking integer truncates.  These produce a tuple of the result value
    // and an overflow bit.
    //
    // TODO: We can/should diagnose statically detectable integer overflow
    // errors and subsume the ConstantFolding.cpp mandatory SIL pass.
    auto IntCheckedTruncFn = [&](bool srcSigned, bool dstSigned)->SymbolicValue{
      auto operandVal = operand.getIntegerValue();
      uint32_t srcBitWidth = operandVal.getBitWidth();
      auto dstBitWidth =
        builtin.Types[1]->castTo<BuiltinIntegerType>()->getGreatestWidth();

      APInt result = operandVal.trunc(dstBitWidth);

      // Compute the overflow by re-extending the value back to its source and
      // checking for loss of value.
      APInt reextended = dstSigned ? result.sext(srcBitWidth)
                                   : result.zext(srcBitWidth);
      bool overflowed = (operandVal != reextended);

      if (builtin.ID == BuiltinValueKind::UToSCheckedTrunc)
        overflowed |= result.isSignBitSet();

      auto &allocator = evaluator.getAllocator();
      // Build the Symbolic value result for our truncated value.
      return SymbolicValue::getAggregate({
          SymbolicValue::getInteger(result, allocator),
          SymbolicValue::getInteger(APInt(1, overflowed), allocator)
      }, allocator);
    };

    switch (builtin.ID) {
    default: break;
    case BuiltinValueKind::SToSCheckedTrunc:
      return IntCheckedTruncFn(true, true);
    case BuiltinValueKind::UToSCheckedTrunc:
      return IntCheckedTruncFn(false, true);
    case BuiltinValueKind::SToUCheckedTrunc:
      return IntCheckedTruncFn(true, false);
    case BuiltinValueKind::UToUCheckedTrunc:
      return IntCheckedTruncFn(false, false);
    case BuiltinValueKind::SIToFP:
    case BuiltinValueKind::UIToFP: {
      auto operandVal = operand.getIntegerValue();
      auto &semantics =
        inst->getType().castTo<BuiltinFloatType>()->getAPFloatSemantics();
      APFloat apf(semantics,
                  APInt::getNullValue(APFloat::semanticsSizeInBits(semantics)));
      apf.convertFromAPInt(operandVal, builtin.ID == BuiltinValueKind::SIToFP,
                           APFloat::rmNearestTiesToEven);
      return SymbolicValue::getFloat(apf, evaluator.getAllocator());
    }

    case BuiltinValueKind::Trunc:
    case BuiltinValueKind::TruncOrBitCast:
    case BuiltinValueKind::ZExt:
    case BuiltinValueKind::ZExtOrBitCast:
    case BuiltinValueKind::SExt:
    case BuiltinValueKind::SExtOrBitCast: {
      unsigned destBitWidth =
        inst->getType().castTo<BuiltinIntegerType>()->getGreatestWidth();

      APInt result = operand.getIntegerValue();
      if (result.getBitWidth() != destBitWidth) {
        switch (builtin.ID) {
        default: assert(0 && "Unknown case");
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
    }
  }

  // Binary operations.
  if (inst->getNumOperands() == 2) {
    auto operand0 = getConstantValue(inst->getOperand(0));
    auto operand1 = getConstantValue(inst->getOperand(1));
    if (!operand0.isConstant()) return operand0;
    if (!operand1.isConstant()) return operand1;

    auto constFoldIntCompare =
      [&](const std::function<bool(const APInt &, const APInt &)> &fn)
        -> SymbolicValue {
      auto result = fn(operand0.getIntegerValue(), operand1.getIntegerValue());
      return SymbolicValue::getInteger(APInt(1, result),
                                       evaluator.getAllocator());
    };
    auto constFoldFPCompare =
      [&](const std::function<bool(APFloat::cmpResult result)> &fn)
        -> SymbolicValue {
      auto comparison =
          operand0.getFloatValue().compare(operand1.getFloatValue());
      return SymbolicValue::getInteger(APInt(1, fn(comparison)),
                                       evaluator.getAllocator());
    };

    switch (builtin.ID) {
    default: break;
#define INT_BINOP(OPCODE, EXPR)                                            \
    case BuiltinValueKind::OPCODE: {                                       \
      auto l = operand0.getIntegerValue(), r = operand1.getIntegerValue(); \
      return SymbolicValue::getInteger((EXPR), evaluator.getAllocator());  \
    }
    INT_BINOP(Add,  l+r)
    INT_BINOP(And,  l&r)
    INT_BINOP(AShr, l.ashr(r))
    INT_BINOP(LShr, l.lshr(r))
    INT_BINOP(Or,   l|r)
    INT_BINOP(Mul,  l*r)
    INT_BINOP(SDiv, l.sdiv(r))
    INT_BINOP(Shl,  l << r)
    INT_BINOP(SRem, l.srem(r))
    INT_BINOP(Sub,  l-r)
    INT_BINOP(UDiv, l.udiv(r))
    INT_BINOP(URem, l.urem(r))
    INT_BINOP(Xor,  l^r)
#undef INT_BINOP
#define FP_BINOP(OPCODE, EXPR)                                           \
    case BuiltinValueKind::OPCODE: {                                     \
      auto l = operand0.getFloatValue(), r = operand1.getFloatValue();   \
      return SymbolicValue::getFloat((EXPR), evaluator.getAllocator());  \
    }
    FP_BINOP(FAdd, l+r)
    FP_BINOP(FSub, l-r)
    FP_BINOP(FMul, l*r)
    FP_BINOP(FDiv, l/r)
    FP_BINOP(FRem, (l.mod(r), l))
#undef FP_BINOP

#define INT_COMPARE(OPCODE, EXPR)                                              \
    case BuiltinValueKind::OPCODE:                                             \
      return constFoldIntCompare([&](const APInt &l, const APInt &r) -> bool { \
        return (EXPR);                                                         \
      })
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
#define FP_COMPARE(OPCODE, EXPR)                                         \
    case BuiltinValueKind::OPCODE:                                       \
      return constFoldFPCompare([&](APFloat::cmpResult result) -> bool { \
        return (EXPR);                                                   \
      })
    FP_COMPARE(FCMP_OEQ, result == APFloat::cmpEqual);
    FP_COMPARE(FCMP_OGT, result == APFloat::cmpGreaterThan);
    FP_COMPARE(FCMP_OGE, result == APFloat::cmpGreaterThan ||
                         result == APFloat::cmpEqual);
    FP_COMPARE(FCMP_OLT, result == APFloat::cmpLessThan);
    FP_COMPARE(FCMP_OLE, result == APFloat::cmpLessThan ||
                         result == APFloat::cmpEqual);
    FP_COMPARE(FCMP_ONE, result == APFloat::cmpLessThan ||
                         result == APFloat::cmpGreaterThan);
    FP_COMPARE(FCMP_ORD, result != APFloat::cmpUnordered);
    FP_COMPARE(FCMP_UEQ, result == APFloat::cmpUnordered ||
                         result == APFloat::cmpEqual);
    FP_COMPARE(FCMP_UGT, result == APFloat::cmpUnordered ||
                         result == APFloat::cmpGreaterThan);
    FP_COMPARE(FCMP_UGE, result != APFloat::cmpLessThan);
    FP_COMPARE(FCMP_ULT, result == APFloat::cmpUnordered ||
                         result == APFloat::cmpLessThan);
    FP_COMPARE(FCMP_ULE, result != APFloat::cmpGreaterThan);
    FP_COMPARE(FCMP_UNE, result != APFloat::cmpEqual);
    FP_COMPARE(FCMP_UNO, result == APFloat::cmpUnordered);
#undef FP_COMPARE
    }
  }


  // Three operand builtins.
  if (inst->getNumOperands() == 3) {
    auto operand0 = getConstantValue(inst->getOperand(0));
    auto operand1 = getConstantValue(inst->getOperand(1));
    auto operand2 = getConstantValue(inst->getOperand(2));
    if (!operand0.isConstant()) return operand0;
    if (!operand1.isConstant()) return operand1;
    if (!operand2.isConstant()) return operand2;

    // Overflowing integer operations like sadd_with_overflow take three
    // operands: the last one is a "should report overflow" bit.
    auto constFoldIntOverflow =
      [&](const std::function<APInt(const APInt &, const APInt &, bool &)> &fn)
            -> SymbolicValue {
      // TODO: We can/should diagnose statically detectable integer overflow
      // errors and subsume the ConstantFolding.cpp mandatory SIL pass.
      auto l = operand0.getIntegerValue(), r = operand1.getIntegerValue();
      bool overflowed = false;
      auto result = fn(l, r, overflowed);
      auto &allocator = evaluator.getAllocator();
      // Build the Symbolic value result for our truncated value.
      return SymbolicValue::getAggregate({
        SymbolicValue::getInteger(result, allocator),
        SymbolicValue::getInteger(APInt(1, overflowed), allocator)
      }, allocator);
    };

    switch (builtin.ID) {
    default: break;

#define INT_OVERFLOW(OPCODE, METHOD)                                   \
    case BuiltinValueKind::OPCODE:                                     \
      return constFoldIntOverflow([&](const APInt &l, const APInt &r,  \
                                      bool &overflowed) -> APInt {     \
        return l.METHOD(r, overflowed);                                \
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

  DEBUG(llvm::errs() << "ConstExpr Unknown Builtin: " << *inst << "\n");

  // Otherwise, we don't know how to handle this builtin.
  return SymbolicValue::getUnknown(SILValue(inst), UnknownReason::Default);
}


/// Given a call to a function, determine whether it is a call to a constexpr
/// function.  If so, collect its arguments as constants, fold it and return
/// None.  If not, mark the results as Unknown, and return an Unknown with
/// information about the error.
llvm::Optional<SymbolicValue>
ConstExprFunctionCache::computeCallResult(ApplyInst *apply) {
  auto conventions = apply->getSubstCalleeConv();

  // The many failure paths through this function invoke this to return their
  // failure information.
  auto failure = [&](UnknownReason reason) -> SymbolicValue {
    auto unknown = SymbolicValue::getUnknown((SILInstruction*)apply, reason);
    // Remember that this call produced unknown as well as any indirect results.
    calculatedValues[apply] = unknown;

    for (unsigned i = 0, e = conventions.getNumIndirectSILResults();
         i != e; ++i) {
      auto resultOperand = apply->getOperand(i+1);
      assert(resultOperand->getType().isAddress() &&
             "Indirect results should be by-address");
      calculatedValues[resultOperand] = unknown;
    }
    return unknown;
  };

  // Determine the callee.
  auto calleeLV = getConstantValue(apply->getOperand(0));
  if (!calleeLV.isConstant())
    return failure(UnknownReason::Default);

  SILFunction *callee = calleeLV.getFunctionValue();

  // If we reached an external function that hasn't been deserialized yet, make
  // sure to pull it in so we can see its body.  If that fails, then we can't
  // analyze the function.
  if (callee->isExternalDeclaration()) {
    callee = initLoader(evaluator.getSILLoader(),
                        callee->getModule()).lookupSILFunction(callee);
    if (!callee || callee->isExternalDeclaration()) {
      DEBUG(llvm::errs() << "ConstExpr Opaque Callee: "
                         << *calleeLV.getFunctionValue() << "\n");
      return failure(UnknownReason::Default);
    }
  }

  // TODO: Verify that the callee was defined as a @constexpr function.

  // Verify that we can fold all of the arguments to the call.
  SmallVector<SymbolicValue, 4> paramConstants;
  unsigned applyParamBaseIndex = 1+conventions.getNumIndirectSILResults();
  auto paramInfos = conventions.getParameters();
  for (unsigned i = 0, e = paramInfos.size(); i != e; ++i) {
    // If any of the arguments is a non-constant value, then we can't fold this
    // call.
    auto cst = getConstantValue(apply->getOperand(applyParamBaseIndex+i));
    if (!cst.isConstant())
      return failure(UnknownReason::Default);

    paramConstants.push_back(cst);
  }

  // Now that have successfully folded all of the parameters, we can evaluate
  // the call.
  SmallVector<SymbolicValue, 4> results;
  auto callResult =
    evaluateAndCacheCall(*callee, apply->getSubstitutions(),
                         paramConstants, results, numInstEvaluated, evaluator);
  if (callResult.hasValue())
    return callResult.getValue();

  unsigned nextResult = 0;

  // If evaluation was successful, remember the results we captured in our
  // current function's cache.
  if (unsigned numNormalResults = conventions.getNumDirectSILResults()) {
    // TODO: unclear when this happens, is this for tuple result values?
    assert(numNormalResults == 1 && "Multiple results aren't supported?");
    calculatedValues[apply->getResults()[0]] = results[nextResult];
    ++nextResult;
  }

  // Handle indirect results as well.
  for (unsigned i = 0, e = conventions.getNumIndirectSILResults(); i != e; ++i){
    calculatedValues[apply->getOperand(1+i)] = results[nextResult];
    ++nextResult;
  }

  assert(nextResult == results.size() && "Unexpected number of results found");

  // We have successfully folded this call!
  return None;
}

/// When analyzing the top-level code involved in a constant expression, we can
/// end up demanding values that are returned by address.  Handle this by
/// finding the temporary stack value that they were stored into and analyzing
/// the single store that should exist into that memory (there are a few forms).
SymbolicValue
ConstExprFunctionCache::computeSingleStoreAddressValue(SILValue addr) {
  // The only value we can otherwise handle is an alloc_stack instruction.
  auto alloc = dyn_cast<AllocStackInst>(addr);
  if (!alloc) return SymbolicValue::getUnknown(addr, UnknownReason::Default);

  // Keep track of the value found for the first constant store.
  SymbolicValue result = SymbolicValue::getUninitMemory();

  // Okay, check out all of the users of this value looking for semantic stores
  // into the address.  If we find more than one, then this was a var or
  // something else we can't handle.
  for (auto *use : alloc->getUses()) {
    auto user = use->getUser();

    // Ignore markers, loads, and other things that aren't stores to this stack
    // value.
    if (isa<LoadInst>(user) ||
        isa<DeallocStackInst>(user) ||
        isa<DebugValueAddrInst>(user))
      continue;

    // TODO: BeginAccess/EndAccess.

#if 0
    // If this is a store *to* the memory, analyze the input value.
    if (auto *si = dyn_cast<StoreInst>(user)) {
      if (use->getOperandNumber() == 1) {
      TODO: implement;
        continue;
      }
    }
#endif
    // TODO: CopyAddr.

    // If this is an apply_inst passing the memory address as an indirect
    // result operand, then we have a call that fills in this result.
    //
    if (auto *apply = dyn_cast<ApplyInst>(user)) {
      auto conventions = apply->getSubstCalleeConv();

      // If this is an out-parameter, it is like a store.  If not, this is an
      // indirect read which is ok.
      unsigned numIndirectResults = conventions.getNumIndirectSILResults();
      unsigned opNum = use->getOperandNumber()-1;
      if (opNum >= numIndirectResults)
        continue;

      // Otherwise this is a write.  If we have already found a value for this
      // stack slot then we're done - we don't support multiple assignment.
      if (result.getKind() != SymbolicValue::UninitMemory)
        return SymbolicValue::getUnknown(addr, UnknownReason::Default);

      // The callee needs to be a direct call to a constant expression.
      assert(!calculatedValues.count(addr) &&
             "Shouldn't already have an entry");
      auto callResult = computeCallResult(apply);

      // If the call failed, we're done.
      if (callResult.hasValue())
        return callResult.getValue();

      // computeCallResult will have figured out the result and cached it for
      // us.
      assert(calculatedValues.count(addr) &&
             calculatedValues[addr].isConstant() &&
             "Should have found a constant result value");
      result = calculatedValues[addr];
      continue;
    }


    DEBUG(llvm::errs() << "Unknown SingleStore ConstExpr user: "
                       << *user << "\n");

    // If this is some other user that we don't know about, then we should
    // treat it conservatively, because it could store into the address.
    return SymbolicValue::getUnknown(addr, UnknownReason::Default);
  }

  // If we found a store of a constant, then return that value!
  if (result.isConstant())
    return result;

  // Otherwise, return unknown.
  return SymbolicValue::getUnknown(addr, UnknownReason::Default);
}


/// Return the symbolic value for the specified SIL value.
SymbolicValue ConstExprFunctionCache::getConstantValue(SILValue value) {
  // Check to see if we already have an answer.
  auto it = calculatedValues.find(value);
  if (it != calculatedValues.end()) return it->second;

  // If the client is asking for the value of a stack object that hasn't been
  // computed, then we are in top level code, and the stack object must be a
  // single store value.  Since this is a very different computation, split it
  // out to its own path.
  if (value->getType().isAddress() && !fn) {
    auto result = computeSingleStoreAddressValue(value);
    return calculatedValues[value] = result;
  }

  // Compute the value of a normal instruction based on its operands.
  auto result = computeConstantValue(value);
  return calculatedValues[value] = result;
}

/// Given an aggregate value like {{1, 2}, 3} and an access path like [0,1], and
/// a scalar like 4, return the aggregate value with the indexed element
/// replaced with its specified scalar, producing {{1, 4}, 3} in this case.
///
/// This returns true on failure and false on success.
///
static bool updateIndexedElement(SymbolicValue &aggregate,
                                 ArrayRef<unsigned> indices,
                                 SymbolicValue scalar,
                                 llvm::BumpPtrAllocator &allocator) {
  // We're done if we've run out of indices.
  if (indices.empty())
    return false;

  // TODO: We should handle updates into uninit memory as well.  TODO: we need
  // to know something about its shape/type to do that because we need to turn
  // it into an aggregate.  Maybe uninit should only be for scalar values?

  if (aggregate.getKind() != SymbolicValue::Aggregate)
    return true;

  // Update the indexed element of the aggregate.
  auto oldElts = aggregate.getAggregateValue();
  SmallVector<SymbolicValue, 4> newElts(oldElts.begin(), oldElts.end());
  if (updateIndexedElement(newElts[indices.front()], indices.drop_front(),
                           scalar, allocator))
    return true;

  aggregate = SymbolicValue::getAggregate(newElts, allocator);
  return false;
}

/// Evaluate the specified instruction in a flow sensitive way, for use by
/// the constexpr function evaluator.  This does not handle control flow
/// statements.  This returns None on success, and an Unknown SymbolicValue with
/// information about an error on failure.
llvm::Optional<SymbolicValue>
ConstExprFunctionCache::evaluateFlowSensitive(SILInstruction *inst) {
  if (isa<DebugValueInst>(inst))
    return None;

  // If this is a special flow-sensitive instruction like a stack allocation,
  // store, copy_addr, etc, we handle it specially here.
  if (auto asi = dyn_cast<AllocStackInst>(inst)) {
    calculatedValues[asi] = SymbolicValue::getUninitMemory();
    return None;
  }

  // If this is a deallocation of a memory object that we may be tracking,
  // remove the memory from the set.  We don't *have* to do this, but it seems
  // useful for hygiene.
  if (isa<DeallocStackInst>(inst)) {
    calculatedValues.erase(inst->getOperand(0));
    return None;
  }

  if (isa<CondFailInst>(inst)) {
    auto failed = getConstantValue(inst->getOperand(0));
    // TODO: Emit a diagnostic if this cond_fail actually fails under constant
    // folding.
    if (failed.isConstant() && failed.getIntegerValue() == 0)
      return None;
  }

  // If this is a call, evaluate it.
  if (auto apply = dyn_cast<ApplyInst>(inst))
    return computeCallResult(apply);

  if (auto *store = dyn_cast<StoreInst>(inst)) {
    auto stored = getConstantValue(inst->getOperand(0));
    if (!stored.isConstant())
      return stored;

    // Only update existing memory locations that we're tracking.
    auto it = calculatedValues.find(inst->getOperand(1));
    if (it == calculatedValues.end())
      return SymbolicValue::getUnknown(inst, UnknownReason::Default);

    // If this is a store to an address, update the element of the base value.
    if (it->second.isAddress()) {
      auto baseVal = getConstantValue(it->second.getAddressBase());
      auto indices = it->second.getAddressIndices();

      if (updateIndexedElement(baseVal, indices, stored,
                               evaluator.getAllocator()))
        return SymbolicValue::getUnknown(inst, UnknownReason::Default);
      stored = baseVal;
    }

    it->second = stored;
    return None;
  }

  // If the instruction produces normal results, try constant folding it.
  // If this fails, then we fail.
  if (inst->getNumResults() != 0) {
    auto result = getConstantValue(inst->getResults()[0]);
    if (result.isConstant()) return None;
    return result;
  }

  DEBUG(llvm::errs() << "ConstExpr Unknown FS: " << *inst << "\n");
  // If this is an unknown instruction with no results then bail out.
  return SymbolicValue::getUnknown(inst, UnknownReason::Default);
}


/// Evaluate a call to the specified function as if it were a constant
/// expression, returning None and filling in `results` on success, or
/// returning an 'Unknown' SymbolicValue on failure carrying the error.
static llvm::Optional<SymbolicValue>
evaluateAndCacheCall(SILFunction &fn, SubstitutionList substitutions,
                     ArrayRef<SymbolicValue> arguments,
                     SmallVectorImpl<SymbolicValue> &results,
                     unsigned &numInstEvaluated,
                     ConstExprEvaluator &evaluator) {
  assert(!fn.isExternalDeclaration() && "Can't analyze bodyless function");
  ConstExprFunctionCache cache(evaluator, &fn, substitutions,
                               numInstEvaluated);

  // TODO: implement caching.
  // TODO: reject code that is too complex.

  // Set up all of the indirect results and argument values.
  auto conventions = fn.getConventions();
  unsigned nextBBArg = 0;
  const auto &argList = fn.front().getArguments();

  for (unsigned i = 0, e = conventions.getNumIndirectSILResults(); i != e; ++i)
    cache.setValue(argList[nextBBArg++], SymbolicValue::getUninitMemory());

  for (auto argument : arguments)
    cache.setValue(argList[nextBBArg++], argument);

  assert(fn.front().getNumArguments() == nextBBArg &&
         "argument count mismatch");

  // Keep track of which blocks we've already visited.  We don't support loops
  // and this allows us to reject them.
  SmallPtrSet<SILBasicBlock*, 8> visitedBlocks;

  // Keep track of the current "instruction pointer".
  SILBasicBlock::iterator nextInst = fn.front().begin();
  visitedBlocks.insert(&fn.front());

  while (1) {
    SILInstruction *inst = &*nextInst++;

    // Make sure we haven't exceeded our interpreter iteration cap.
    if (++numInstEvaluated > ConstExprLimit)
      return SymbolicValue::getUnknown(inst,
                                       UnknownReason::TooManyInstructions);

    // If we can evaluate this flow sensitively, then keep going.
    if (!isa<TermInst>(inst)) {
      auto fsResult = cache.evaluateFlowSensitive(inst);
      if (fsResult.hasValue())
        return fsResult;
      continue;
    }

    // Otherwise, we handle terminators here.
    if (isa<ReturnInst>(inst)) {
      auto val = cache.getConstantValue(inst->getOperand(0));
      if (!val.isConstant())
        return val;

      // If we got a constant value, then we're good.  Set up the normal result
      // values as well any indirect results.
      auto numNormalResults = conventions.getNumDirectSILResults();
      if (numNormalResults == 1) {
        results.push_back(val);
      } else if (numNormalResults > 1) {
        auto elts = val.getAggregateValue();
        assert(elts.size() == numNormalResults && "result list mismatch!");
        results.append(results.begin(), results.end());
      }

      for (unsigned i = 0, e = conventions.getNumIndirectSILResults();
           i != e; ++i) {
        auto result = cache.getConstantValue(argList[i]);
        if (!result.isConstant())
          return result;
        results.push_back(result);
      }

      // TODO: Handle caching of results.
      return None;
    }

    if (auto *br = dyn_cast<BranchInst>(inst)) {
      auto destBB = br->getDestBB();

      // If we've already visited this block then fail - we have a loop.
      if (!visitedBlocks.insert(destBB).second)
        return SymbolicValue::getUnknown(br, UnknownReason::Default);

      // Set up basic block arguments.
      for (unsigned i = 0, e = br->getNumArgs(); i != e; ++i) {
        auto argument = cache.getConstantValue(destBB->getArgument(i));
        if (!argument.isConstant()) return argument;
        cache.setValue(br->getArg(i), argument);
      }
      // Set the instruction pointer to the first instruction of the block.
      nextInst = destBB->begin();
      continue;
    }

    if (auto *cbr = dyn_cast<CondBranchInst>(inst)) {
      auto val = cache.getConstantValue(inst->getOperand(0));
      if (!val.isConstant()) return val;

      SILBasicBlock *destBB;
      if (!val.getIntegerValue())
        destBB = cbr->getFalseBB();
      else
        destBB = cbr->getTrueBB();

      // If we've already visited this block then fail - we have a loop.
      if (!visitedBlocks.insert(destBB).second)
        return SymbolicValue::getUnknown(cbr, UnknownReason::Default);

      nextInst = destBB->begin();
      continue;
    }

    DEBUG(llvm::errs() << "ConstExpr: Unknown Terminator: " << *inst << "\n");

    // TODO: Enum switches when we support enums?
    return SymbolicValue::getUnknown(inst, UnknownReason::Default);
  }
}

//===----------------------------------------------------------------------===//
// ConstExprEvaluator implementation.
//===----------------------------------------------------------------------===//

ConstExprEvaluator::ConstExprEvaluator(SILModule &m) {
}

ConstExprEvaluator::~ConstExprEvaluator() {
}

/// Analyze the specified values to determine if they are constant values.  This
/// is done in code that is not necessarily itself a constexpr function.  The
/// results are added to the results list which is a parallel structure to the
/// input values.
///
/// TODO: Return information about which callees were found to be
/// constexprs, which would allow the caller to delete dead calls to them
/// that occur after after folding them.
void ConstExprEvaluator::
computeConstantValues(ArrayRef<SILValue> values,
                      SmallVectorImpl<SymbolicValue> &results) {
  unsigned numInstEvaluated = 0;
  ConstExprFunctionCache cache(*this, nullptr, {}, numInstEvaluated);
  for (auto v : values) {
    auto symVal = cache.getConstantValue(v);
    results.push_back(symVal);

    // Reset the execution limit back to zero for each subsexpression we look
    // at.  We don't want lots of constants folded to trigger a limit.
    numInstEvaluated = 0;
  }
}
