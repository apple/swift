//===--- DifferentiableActivityAnalysis.h ----------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "differentiation"

#include "swift/SILOptimizer/Analysis/DifferentiableActivityAnalysis.h"
#include "swift/SILOptimizer/Differentiation/Common.h"

#include "swift/SIL/Projection.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"

using namespace swift;
using namespace swift::autodiff;

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

static bool isWithoutDerivative(SILValue v) {
  if (auto *fnRef = dyn_cast<FunctionRefInst>(v))
    return fnRef->getReferencedFunction()->hasSemanticsAttr(
        "autodiff.nonvarying");
  return false;
}

std::unique_ptr<DifferentiableActivityCollection>
DifferentiableActivityAnalysis::newFunctionAnalysis(SILFunction *f) {
  assert(dominanceAnalysis && "Expect a valid dominance analysis");
  assert(postDominanceAnalysis && "Expect a valid post-dominance analysis");
  return std::make_unique<DifferentiableActivityCollection>(
      *f, dominanceAnalysis->get(f), postDominanceAnalysis->get(f));
}

void DifferentiableActivityAnalysis::initialize(SILPassManager *pm) {
  dominanceAnalysis = pm->getAnalysis<DominanceAnalysis>();
  postDominanceAnalysis = pm->getAnalysis<PostDominanceAnalysis>();
}

SILAnalysis *swift::createDifferentiableActivityAnalysis(SILModule *m) {
  return new DifferentiableActivityAnalysis();
}

DifferentiableActivityCollection::DifferentiableActivityCollection(
    SILFunction &f, DominanceInfo *di, PostDominanceInfo *pdi)
    : function(f), domInfo(di), postDomInfo(pdi) {}

DifferentiableActivityInfo::DifferentiableActivityInfo(
    DifferentiableActivityCollection &parent, GenericSignature derivGenSig)
    : parent(parent), derivativeGenericSignature(derivGenSig) {
  analyze(parent.domInfo, parent.postDomInfo);
}

SILFunction &DifferentiableActivityInfo::getFunction() const {
  return parent.function;
}

void DifferentiableActivityInfo::analyze(DominanceInfo *di,
                                         PostDominanceInfo *pdi) {
  auto &function = getFunction();
  LLVM_DEBUG(getADDebugStream()
             << "Running activity analysis on @" << function.getName() << '\n');
  // Inputs are just function's arguments, count `n`.
  auto paramArgs = function.getArgumentsWithoutIndirectResults();
  for (auto value : paramArgs)
    inputValues.push_back(value);
  LLVM_DEBUG({
    auto &s = getADDebugStream();
    s << "Inputs in @" << function.getName() << ":\n";
    for (auto val : inputValues)
      s << val << '\n';
  });
  // Outputs are indirect result buffers and return values, count `m`.
  collectAllFormalResultsInTypeOrder(function, outputValues);
  LLVM_DEBUG({
    auto &s = getADDebugStream();
    s << "Outputs in @" << function.getName() << ":\n";
    for (auto val : outputValues)
      s << val << '\n';
  });

  // Propagate variedness starting from the inputs.
  assert(variedValueSets.empty());
  for (auto inputAndIdx : enumerate(inputValues)) {
    auto input = inputAndIdx.value();
    unsigned i = inputAndIdx.index();
    variedValueSets.push_back({});
    setVariedAndPropagateToUsers(input, i);
  }

  // Mark differentiable outputs as useful.
  assert(usefulValueSets.empty());
  for (auto outputAndIdx : enumerate(outputValues)) {
    auto output = outputAndIdx.value();
    unsigned i = outputAndIdx.index();
    usefulValueSets.push_back({});
    setUsefulAndPropagateToOperands(output, i);
  }
}

void DifferentiableActivityInfo::setVariedAndPropagateToUsers(
    SILValue value, unsigned independentVariableIndex) {
  // Skip already-varied values to prevent infinite recursion.
  if (isVaried(value, independentVariableIndex))
    return;
  // Set the value as varied.
  variedValueSets[independentVariableIndex].insert(value);
  // Propagate variedness to users.
  for (auto *use : value->getUses())
    propagateVaried(use, independentVariableIndex);
}

void DifferentiableActivityInfo::propagateVaried(
    Operand *operand, unsigned independentVariableIndex) {
  auto *inst = operand->getUser();
  // Propagate variedness for the given operand.
  // General rule: mark results as varied and recursively propagate variedness
  // to users of results.
  auto i = independentVariableIndex;
  // Handle full apply sites: `apply`, `try_apply`, and `begin_apply`.
  if (FullApplySite::isa(inst)) {
    FullApplySite applySite(inst);
    // Skip non-varying callees.
    if (isWithoutDerivative(applySite.getCallee()))
      return;
    // If operand is varied, set all direct/indirect results and inout arguments
    // as varied.
    if (isVaried(operand->get(), i)) {
      for (auto indRes : applySite.getIndirectSILResults())
        propagateVariedInwardsThroughProjections(indRes, i);
      for (auto semresArg : applySite.getAutoDiffSemanticResultArguments())
        propagateVariedInwardsThroughProjections(semresArg, i);
      // Propagate variedness to apply site direct results.
      forEachApplyDirectResult(applySite, [&](SILValue directResult) {
        setVariedAndPropagateToUsers(directResult, i);
      });
    }
  }
  // Handle store-like instructions:
  //   `store`, `store_borrow`, `copy_addr`, `unconditional_checked_cast`
#define PROPAGATE_VARIED_THROUGH_STORE(INST)                                   \
  else if (auto *si = dyn_cast<INST##Inst>(inst)) {                            \
    if (isVaried(si->getSrc(), i))                                             \
      propagateVariedInwardsThroughProjections(si->getDest(), i);              \
  }
  PROPAGATE_VARIED_THROUGH_STORE(Store)
  PROPAGATE_VARIED_THROUGH_STORE(CopyAddr)
  PROPAGATE_VARIED_THROUGH_STORE(UnconditionalCheckedCastAddr)
#undef PROPAGATE_VARIED_THROUGH_STORE
  else if (auto *sbi = dyn_cast<StoreBorrowInst>(inst)) {
    if (isVaried(sbi->getSrc(), i)) {
      setVariedAndPropagateToUsers(sbi, i);
      propagateVariedInwardsThroughProjections(sbi, i);
    }
  }
  // Handle `tuple_element_addr`.
  else if (auto *teai = dyn_cast<TupleElementAddrInst>(inst)) {
    if (isVaried(teai->getOperand(), i)) {
      // Propagate variedness only if the `tuple_element_addr` result has a
      // tangent space. Otherwise, the result does not need a derivative.
      if (hasTangentSpace(teai))
        setVariedAndPropagateToUsers(teai, i);
    }
  }
  // Handle element projection instructions:
  //   `struct_extract`, `struct_element_addr`, `ref_element_addr`.
  // - If the field is marked `@noDerivative`, do not set the result as
  //   varied because it does not need a derivative.
  // - Otherwise, propagate variedness from operand to result as usual.
#define PROPAGATE_VARIED_FOR_ELEMENT_PROJECTION(INST)                          \
  else if (auto *projInst = dyn_cast<INST##Inst>(inst)) {                      \
    if (isVaried(projInst->getOperand(), i) &&                                 \
        !projInst->getField()->getAttrs().hasAttribute<NoDerivativeAttr>())    \
      setVariedAndPropagateToUsers(projInst, i);                               \
  }
  PROPAGATE_VARIED_FOR_ELEMENT_PROJECTION(StructExtract)
  PROPAGATE_VARIED_FOR_ELEMENT_PROJECTION(StructElementAddr)
  PROPAGATE_VARIED_FOR_ELEMENT_PROJECTION(RefElementAddr)
#undef PROPAGATE_VARIED_FOR_STRUCT_EXTRACTION
  // Handle `br`.
  else if (auto *bi = dyn_cast<BranchInst>(inst)) {
    if (isVaried(operand->get(), i))
      setVariedAndPropagateToUsers(bi->getArgForOperand(operand), i);
  }
  // Handle `cond_br`.
  else if (auto *cbi = dyn_cast<CondBranchInst>(inst)) {
    if (isVaried(operand->get(), i))
      if (auto *destBBArg = cbi->getArgForOperand(operand))
        setVariedAndPropagateToUsers(destBBArg, i);
  }
  // Handle `checked_cast_addr_br`.
  // Propagate variedness from source operand to destination operand, in
  // addition to all successor block arguments.
  else if (auto *ccabi = dyn_cast<CheckedCastAddrBranchInst>(inst)) {
    if (isVaried(ccabi->getSrc(), i)) {
      setVariedAndPropagateToUsers(ccabi->getDest(), i);
      for (auto *succBB : ccabi->getSuccessorBlocks())
        for (auto *arg : succBB->getArguments())
          setVariedAndPropagateToUsers(arg, i);
    }
  }
  // Handle all other terminators: if any operand is active, propagate
  // variedness to all successor block arguments. This logic may be incorrect
  // for some terminator instructions, so special cases must be defined above.
  else if (auto *termInst = dyn_cast<TermInst>(inst)) {
    for (auto &op : termInst->getAllOperands())
      if (isVaried(op.get(), i))
        for (auto *succBB : termInst->getSuccessorBlocks())
          for (auto *arg : succBB->getArguments())
            setVariedAndPropagateToUsers(arg, i);
  }
  // Handle everything else.
  else {
    for (auto &op : inst->getAllOperands()) {
      if (isVaried(op.get(), i))
        for (auto result : inst->getResults())
          setVariedAndPropagateToUsers(result, i);
    }
  }
}

/// Returns the accessor kind of the given SIL function, if it is a lowered
/// accessor. Otherwise, return `None`.
static llvm::Optional<AccessorKind> getAccessorKind(SILFunction *fn) {
  auto *dc = fn->getDeclContext();
  if (!dc)
    return llvm::None;
  auto *accessor = dyn_cast_or_null<AccessorDecl>(dc->getAsDecl());
  if (!accessor)
    return llvm::None;
  return accessor->getAccessorKind();
}

void DifferentiableActivityInfo::propagateVariedInwardsThroughProjections(
    SILValue value, unsigned independentVariableIndex) {
  auto i = independentVariableIndex;
  // Skip `@noDerivative` projections.
#define SKIP_NODERIVATIVE(INST)                                                \
  if (auto *projInst = dyn_cast<INST##Inst>(value))                            \
    if (projInst->getField()->getAttrs().hasAttribute<NoDerivativeAttr>())     \
      return;
  SKIP_NODERIVATIVE(StructExtract)
  SKIP_NODERIVATIVE(StructElementAddr)
  SKIP_NODERIVATIVE(RefElementAddr)
#undef SKIP_NODERIVATIVE
  // Set value as varied and propagate to users.
  setVariedAndPropagateToUsers(value, i);
  auto *inst = value->getDefiningInstruction();
  if (!inst)
    return;
  if (ApplySite::isa(inst)) {
    ApplySite applySite(inst);
    // If callee is non-varying, skip.
    if (isWithoutDerivative(applySite.getCallee()))
      return;
    // If callee is a `modify` accessor, propagate variedness from yielded
    // addresses to `inout` arguments. Semantically, yielded addresses can be
    // viewed as a projection into the `inout` argument.
    // Note: the assumption that yielded addresses are always a projection into
    // the `inout` argument is a safe over-approximation but not always true.
    if (auto *bai = dyn_cast<BeginApplyInst>(inst)) {
      if (auto *calleeFn = bai->getCalleeFunction()) {
        if (getAccessorKind(calleeFn) == AccessorKind::Modify) {
          for (auto inoutArg : bai->getInoutArguments())
            propagateVariedInwardsThroughProjections(inoutArg, i);
        }
      }
    }
    return;
  }
  // Default: propagate variedness through projections to the operands of their
  // defining instructions. This handles projections like:
  // - `struct_element_addr`
  // - `tuple_element_addr`
  for (auto &op : inst->getAllOperands())
    propagateVariedInwardsThroughProjections(op.get(), i);
}

void DifferentiableActivityInfo::setUseful(SILValue value,
                                           unsigned dependentVariableIndex) {
  usefulValueSets[dependentVariableIndex].insert(value);
  setUsefulThroughArrayInitialization(value, dependentVariableIndex);
}

void DifferentiableActivityInfo::setUsefulAndPropagateToOperands(
    SILValue value, unsigned dependentVariableIndex) {
  // Skip already-useful values to prevent infinite recursion.
  if (isUseful(value, dependentVariableIndex))
    return;
  if (value->getType().isAddress() ||
      value->getType().getClassOrBoundGenericClass()) {
    propagateUsefulThroughAddress(value, dependentVariableIndex);
    return;
  }
  setUseful(value, dependentVariableIndex);

  // If the given value is a basic block argument, propagate usefulness to
  // incoming values.
  if (auto *bbArg = dyn_cast<SILPhiArgument>(value)) {
    SmallVector<SILValue, 4> incomingValues;
    if (bbArg->getSingleTerminatorOperands(incomingValues)) {
      for (auto incomingValue : incomingValues)
        setUsefulAndPropagateToOperands(incomingValue, dependentVariableIndex);
      return;
    } else if (bbArg->isTerminatorResult()) {
      if (TryApplyInst *tai = dyn_cast<TryApplyInst>(bbArg->getTerminatorForResult())) {
        propagateUseful(tai, dependentVariableIndex);
        return;
      } else
        llvm::report_fatal_error("unknown terminator with result");
   } else
      llvm::report_fatal_error("do not know how to handle this incoming bb argument");
  }
  
  auto *inst = value->getDefiningInstruction();
  if (!inst)
    return;
  propagateUseful(inst, dependentVariableIndex);
}

void DifferentiableActivityInfo::propagateUseful(
    SILInstruction *inst, unsigned dependentVariableIndex) {
  // Propagate usefulness for the given instruction: mark operands as useful and
  // recursively propagate usefulness to defining instructions of operands.
  auto i = dependentVariableIndex;
  // Handle full apply sites: `apply`, `try_apply`, and `begin_apply`.
  if (FullApplySite::isa(inst)) {
    FullApplySite applySite(inst);
    // If callee is non-varying, skip.
    if (isWithoutDerivative(applySite.getCallee()))
      return;
    // If callee is a `modify` accessor, propagate usefulness through yielded
    // addresses. Semantically, yielded addresses can be viewed as a projection
    // into the `inout` argument.
    // Note: the assumption that yielded addresses are always a projection into
    // the `inout` argument is a safe over-approximation but not always true.
    if (auto *bai = dyn_cast<BeginApplyInst>(inst)) {
      if (auto *calleeFn = bai->getCalleeFunction())
        if (getAccessorKind(calleeFn) == AccessorKind::Modify)
          for (auto yield : bai->getYieldedValues())
            setUsefulAndPropagateToOperands(yield, i);
    }
    // Propagate usefulness through apply site arguments.
    for (auto arg : applySite.getArgumentsWithoutIndirectResults())
      setUsefulAndPropagateToOperands(arg, i);
  }
  // Handle store-like instructions:
  //   `store`, `store_borrow`, `copy_addr`, `unconditional_checked_cast`
#define PROPAGATE_USEFUL_THROUGH_STORE(INST)                                   \
  else if (auto *si = dyn_cast<INST##Inst>(inst)) {                            \
    setUsefulAndPropagateToOperands(si->getSrc(), i);                          \
  }
  PROPAGATE_USEFUL_THROUGH_STORE(Store)
  PROPAGATE_USEFUL_THROUGH_STORE(StoreBorrow)
  PROPAGATE_USEFUL_THROUGH_STORE(CopyAddr)
  PROPAGATE_USEFUL_THROUGH_STORE(UnconditionalCheckedCastAddr)
#undef PROPAGATE_USEFUL_THROUGH_STORE
  // Handle element projections, skipping `@noDerivative` fields:
  //   `struct_extract`, `struct_element_addr`, `ref_element_addr`.
#define PROPAGATE_USEFUL_THROUGH_ELEMENT_PROJECTION(INST)                      \
  else if (auto *projInst = dyn_cast<INST##Inst>(inst)) {                      \
    if (!projInst->getField()->getAttrs().hasAttribute<NoDerivativeAttr>())    \
      setUsefulAndPropagateToOperands(projInst->getOperand(), i);              \
  }
  PROPAGATE_USEFUL_THROUGH_ELEMENT_PROJECTION(StructExtract)
  PROPAGATE_USEFUL_THROUGH_ELEMENT_PROJECTION(StructElementAddr)
  PROPAGATE_USEFUL_THROUGH_ELEMENT_PROJECTION(RefElementAddr)
#undef PROPAGATE_USEFUL_THROUGH_STRUCT_EXTRACTION
  // Handle everything else.
  else {
    for (auto &op : inst->getAllOperands())
      setUsefulAndPropagateToOperands(op.get(), i);
  }
}

void DifferentiableActivityInfo::propagateUsefulThroughAddress(
    SILValue value, unsigned dependentVariableIndex) {
  assert(value->getType().isAddress() ||
         value->getType().getClassOrBoundGenericClass());
  // Skip already-useful values to prevent infinite recursion.
  if (isUseful(value, dependentVariableIndex))
    return;
  setUseful(value, dependentVariableIndex);
  if (auto *inst = value->getDefiningInstruction())
    propagateUseful(inst, dependentVariableIndex);
  // Recursively propagate usefulness through users that are projections or
  // `begin_access` instructions.
  for (auto use : value->getUses()) {
    // Propagate usefulness through user's operands.
    propagateUseful(use->getUser(), dependentVariableIndex);
    for (auto res : use->getUser()->getResults()) {
#define SKIP_NODERIVATIVE(INST)                                                \
  if (auto *projInst = dyn_cast<INST##Inst>(res))                              \
    if (projInst->getField()->getAttrs().hasAttribute<NoDerivativeAttr>())     \
      continue;
      SKIP_NODERIVATIVE(StructExtract)
      SKIP_NODERIVATIVE(StructElementAddr)
      SKIP_NODERIVATIVE(RefElementAddr)
#undef SKIP_NODERIVATIVE
      if (Projection::isAddressProjection(res) || isa<BeginAccessInst>(res) ||
          isa<BeginBorrowInst>(res))
        propagateUsefulThroughAddress(res, dependentVariableIndex);
    }
  }
}

void DifferentiableActivityInfo::setUsefulThroughArrayInitialization(
    SILValue value, unsigned dependentVariableIndex) {
  // Array initializer syntax is lowered to an intrinsic and one or more
  // stores to a `RawPointer` returned by the intrinsic.
  ArraySemanticsCall uninitCall(value,
                                semantics::ARRAY_UNINITIALIZED_INTRINSIC);
  ApplyInst *uai = uninitCall;
  if (!uai)
    return;
  for (auto use : value->getUses()) {
    auto *dti = dyn_cast<DestructureTupleInst>(use->getUser());
    if (!dti)
      continue;
    // The second tuple field of the return value is the `RawPointer`.
    for (auto use : dti->getResult(1)->getUses()) {
      // The `RawPointer` passes through a `pointer_to_address`. That
      // instruction's first use is a `store` whose source is useful; its
      // subsequent uses are `index_addr`s whose only use is a useful `store`.
      auto *ptai = dyn_cast<PointerToAddressInst>(use->getUser());
      assert(ptai && "Expected `pointer_to_address` user for uninitialized "
                     "array intrinsic");
      setUseful(ptai, dependentVariableIndex);
      // Propagate usefulness through array element addresses:
      // `pointer_to_address` and `index_addr` instructions.
      //
      // - Set all array element addresses as useful.
      // - Find instructions with array element addresses as "result":
      //   - `store` and `copy_addr` with array element address as destination.
      //   - `apply` with array element address as an indirect result.
      // - For each instruction, propagate usefulness through "arguments":
      //   - `store` and `copy_addr`: propagate to source.
      //   - `apply`: propagate to arguments.
      //
      // NOTE: `propagateUseful(use->getUser(), ...)` is intentionally not used
      // because it marks more values than necessary as useful, including:
      // - The `RawPointer` result of the intrinsic.
      // - `integer_literal` operands to `index_addr` for indexing the
      //   `RawPointer`.
      // It is also blocked by TF-1032: control flow differentiation crash for
      // active values with no tangent space.
      for (auto use : ptai->getUses()) {
        auto *user = use->getUser();
        if (auto *si = dyn_cast<StoreInst>(user)) {
          setUseful(si->getDest(), dependentVariableIndex);
          setUsefulAndPropagateToOperands(si->getSrc(), dependentVariableIndex);
        } else if (auto *cai = dyn_cast<CopyAddrInst>(user)) {
          setUseful(cai->getDest(), dependentVariableIndex);
          setUsefulAndPropagateToOperands(cai->getSrc(),
                                          dependentVariableIndex);
        } else if (auto *ai = dyn_cast<ApplyInst>(user)) {
          if (FullApplySite(ai).isIndirectResultOperand(*use))
            for (auto arg : ai->getArgumentsWithoutIndirectResults())
              setUsefulAndPropagateToOperands(arg, dependentVariableIndex);
        } else if (auto *iai = dyn_cast<IndexAddrInst>(user)) {
          setUseful(iai, dependentVariableIndex);
          for (auto use : iai->getUses()) {
            auto *user = use->getUser();
            if (auto si = dyn_cast<StoreInst>(user)) {
              setUseful(si->getDest(), dependentVariableIndex);
              setUsefulAndPropagateToOperands(si->getSrc(),
                                              dependentVariableIndex);
            } else if (auto *cai = dyn_cast<CopyAddrInst>(user)) {
              setUseful(cai->getDest(), dependentVariableIndex);
              setUsefulAndPropagateToOperands(cai->getSrc(),
                                              dependentVariableIndex);
            } else if (auto *ai = dyn_cast<ApplyInst>(user)) {
              if (FullApplySite(ai).isIndirectResultOperand(*use))
                for (auto arg : ai->getArgumentsWithoutIndirectResults())
                  setUsefulAndPropagateToOperands(arg, dependentVariableIndex);
            }
          }
        }
      }
    }
  }
}

bool DifferentiableActivityInfo::isVaried(
    SILValue value, unsigned independentVariableIndex) const {
  assert(independentVariableIndex < variedValueSets.size() &&
         "Independent variable index out of range");
  auto &set = variedValueSets[independentVariableIndex];
  return set.count(value);
}

bool DifferentiableActivityInfo::isVaried(
    SILValue value, IndexSubset *independentVariableIndices) const {
  for (auto i : independentVariableIndices->getIndices())
    if (isVaried(value, i))
      return true;
  return false;
}

bool DifferentiableActivityInfo::isUseful(
    SILValue value, unsigned dependentVariableIndex) const {
  assert(dependentVariableIndex < usefulValueSets.size() &&
         "Dependent variable index out of range");
  auto &set = usefulValueSets[dependentVariableIndex];
  return set.count(value);
}

bool DifferentiableActivityInfo::isUseful(
    SILValue value, IndexSubset *dependentVariableIndices) const {
  for (auto i : dependentVariableIndices->getIndices())
    if (isUseful(value, i))
      return true;
  return false;
}

bool DifferentiableActivityInfo::isActive(
    SILValue value, IndexSubset *parameterIndices,
    IndexSubset *resultIndices) const {
  return isVaried(value, parameterIndices) && isUseful(value, resultIndices);
}

Activity DifferentiableActivityInfo::getActivity(
    SILValue value, IndexSubset *parameterIndices,
    IndexSubset *resultIndices) const {
  Activity activity;
  if (isVaried(value, parameterIndices))
    activity |= ActivityFlags::Varied;
  if (isUseful(value, resultIndices))
    activity |= ActivityFlags::Useful;
  return activity;
}

void DifferentiableActivityInfo::dump(
    SILValue value, IndexSubset *parameterIndices, IndexSubset *resultIndices,
    llvm::raw_ostream &s) const {
  s << '[';
  auto activity = getActivity(value, parameterIndices, resultIndices);
  switch (activity.toRaw()) {
  case 0:
    s << "NONE";
    break;
  case (unsigned)ActivityFlags::Varied:
    s << "VARIED";
    break;
  case (unsigned)ActivityFlags::Useful:
    s << "USEFUL";
    break;
  case (unsigned)ActivityFlags::Active:
    s << "ACTIVE";
    break;
  }
  s << "] " << value;
}

void DifferentiableActivityInfo::dump(
    IndexSubset *parameterIndices, IndexSubset *resultIndices,
    llvm::raw_ostream &s) const {
  SILFunction &fn = getFunction();
  s << "Activity info for " << fn.getName() << " at parameter indices (";
  llvm::interleaveComma(parameterIndices->getIndices(), s);
  s << ") and result indices (";
  llvm::interleaveComma(resultIndices->getIndices(), s);
  s << "):\n";
  for (auto &bb : fn) {
    s << "bb" << bb.getDebugID() << ":\n";
    for (auto *arg : bb.getArguments())
      dump(arg, parameterIndices, resultIndices, s);
    for (auto &inst : bb)
      for (auto res : inst.getResults())
        dump(res, parameterIndices, resultIndices, s);
    if (std::next(bb.getIterator()) != fn.end())
      s << '\n';
  }
  s << "End activity info for " << fn.getName() << '\n';
}
