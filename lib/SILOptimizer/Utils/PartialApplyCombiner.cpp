//===--- PartialApplyCombiner.cpp -----------------------------------------===//
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

#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"

using namespace swift;

namespace {

// Helper class performing the apply{partial_apply(x,y)}(z) -> apply(z,x,y)
// peephole.
class PartialApplyCombiner {
  // partial_apply which is being processed.
  PartialApplyInst *pai;

  // Temporaries created as copies of alloc_stack arguments of
  // the partial_apply.
  SmallVector<SILValue, 8> tmpCopies;

  // Mapping from the original argument of partial_apply to
  // the temporary containing its copy.
  llvm::DenseMap<SILValue, SILValue> argToTmpCopy;

  SILBuilderContext &builderCtxt;

  InstModCallbacks &callbacks;

  bool copyArgsToTemporaries(ArrayRef<FullApplySite> applies);

  void processSingleApply(FullApplySite ai);

public:
  PartialApplyCombiner(PartialApplyInst *pai, SILBuilderContext &builderCtxt,
                       InstModCallbacks &callbacks)
      : pai(pai), builderCtxt(builderCtxt), callbacks(callbacks) {}

  bool combine();
};

} // end anonymous namespace

/// Copy the original arguments of the partial_apply into newly created
/// temporaries and use these temporaries instead of the original arguments
/// afterwards.
///
/// This is done to "extend" the life-time of original partial_apply arguments,
/// as they may be destroyed/deallocated before the last use by one of the
/// apply instructions.
bool PartialApplyCombiner::copyArgsToTemporaries(
    ArrayRef<FullApplySite> applies) {
  // A partial_apply [stack]'s argument are not owned by the partial_apply and
  // therefore their lifetime must outlive any uses.
  if (pai->isOnStack())
    return true;

  SmallVector<Operand *, 8> argsToHandle;
  getConsumedPartialApplyArgs(pai, argsToHandle,
                              /*includeTrivialAddrArgs*/ true);
  if (argsToHandle.empty())
    return true;

  // Compute the set of endpoints, which will be used to insert destroys of
  // temporaries.
  SmallVector<SILInstruction *, 16> paiUsers;

  // Of course we must inlude all apply instructions which we want to optimize.
  for (FullApplySite ai : applies) {
    paiUsers.push_back(ai.getInstruction());
  }

  // Also include all destroys in the liferange for the arguments.
  // This is needed for later processing in tryDeleteDeadClosure: in case the
  // pai gets dead after this optimization, tryDeleteDeadClosure relies on that
  // we already copied the pai arguments to extend their lifetimes until the pai
  // is finally destroyed.
  collectDestroys(pai, paiUsers);

  ValueLifetimeAnalysis vla(pai, paiUsers);
  ValueLifetimeAnalysis::Frontier partialApplyFrontier;

  // Computing the frontier may fail if the frontier is located on a critical
  // edge which we may not split.
  if (!vla.computeFrontier(partialApplyFrontier,
                           ValueLifetimeAnalysis::DontModifyCFG)) {
    return false;
  }

  for (Operand *argOp : argsToHandle) {
    SILValue arg = argOp->get();
    int argIdx = ApplySite(pai).getAppliedArgIndex(*argOp);
    SILDebugVariable dbgVar(/*Constant*/ true, argIdx);

    SILValue tmp = arg;
    SILBuilderWithScope builder(pai, builderCtxt);
    if (arg->getType().isObject()) {
      tmp = builder.emitCopyValueOperation(pai->getLoc(), arg);
    } else {
      // Copy address-arguments into a stack-allocated temporary.
      tmp = builder.createAllocStack(pai->getLoc(), arg->getType(), dbgVar);
      builder.createCopyAddr(pai->getLoc(), arg, tmp, IsTake_t::IsNotTake,
                             IsInitialization_t::IsInitialization);
    }
    argToTmpCopy.insert(std::make_pair(arg, tmp));

    // Destroy the argument value (either as SSA value or in the stack-
    // allocated temporary) at the end of the partial_apply's lifetime.
    endLifetimeAtFrontier(tmp, partialApplyFrontier, builderCtxt, callbacks);
  }
  return true;
}

/// Process an apply instruction which uses a partial_apply
/// as its callee.
/// Returns true on success.
void PartialApplyCombiner::processSingleApply(FullApplySite paiAI) {
  // The arguments of the final apply instruction.
  SmallVector<SILValue, 8> argList;
  // First, add the arguments of ther original ApplyInst args.
  for (auto Op : paiAI.getArguments())
    argList.push_back(Op);

  SILBuilderWithScope builder(paiAI.getInstruction(), builderCtxt);

  // The thunk that implements the partial apply calls the closure function
  // that expects all arguments to be consumed by the function. However, the
  // captured arguments are not arguments of *this* apply, so they are not
  // pre-incremented. When we combine the partial_apply and this apply into
  // a new apply we need to retain all of the closure non-address type
  // arguments.
  auto destroyloc = RegularLocation::getAutoGeneratedLocation();
  auto paramInfo = pai->getSubstCalleeType()->getParameters();
  auto partialApplyArgs = pai->getArguments();
  for (unsigned i : indices(partialApplyArgs)) {
    SILValue arg = partialApplyArgs[i];
    if (argToTmpCopy.count(arg))
      arg = argToTmpCopy.lookup(arg);

    if (paramInfo[paramInfo.size() - partialApplyArgs.size() + i]
            .isConsumed()) {
      // Copy the argument as the callee may consume it.
      if (arg->getType().isAddress()) {
        auto *ASI = builder.createAllocStack(pai->getLoc(), arg->getType());
        builder.createCopyAddr(pai->getLoc(), arg, ASI, IsTake_t::IsNotTake,
                               IsInitialization_t::IsInitialization);
        paiAI.insertAfterFullEvaluation([&](SILBuilder &builder) {
          builder.createDeallocStack(destroyloc, ASI);
        });
        arg = ASI;
      } else {
        arg = builder.emitCopyValueOperation(pai->getLoc(), arg);
      }
    }
    // Add the argument of the partial_apply.
    argList.push_back(arg);
  }

  SILValue callee = pai->getCallee();
  SubstitutionMap subs = pai->getSubstitutionMap();

  // The partial_apply might be substituting in an open existential type.
  builder.addOpenedArchetypeOperands(pai);

  if (auto *tai = dyn_cast<TryApplyInst>(paiAI)) {
    builder.createTryApply(paiAI.getLoc(), callee, subs, argList,
                           tai->getNormalBB(), tai->getErrorBB());
  } else {
    auto *apply = cast<ApplyInst>(paiAI);
    auto *newAI = builder.createApply(paiAI.getLoc(), callee, subs, argList,
                                      apply->isNonThrowing());
    callbacks.replaceValueUsesWith(apply, newAI);
  }
  // We also need to destroy the partial_apply instruction itself because it is
  // consumed by the apply_instruction.
  if (!pai->hasCalleeGuaranteedContext()) {
    paiAI.insertAfterFullEvaluation([&](SILBuilder &builder) {
      builder.emitDestroyValueOperation(destroyloc, pai);
    });
  }
  callbacks.deleteInst(paiAI.getInstruction());
}

/// Perform the apply{partial_apply(x,y)}(z) -> apply(z,x,y) peephole
/// by iterating over all uses of the partial_apply and searching
/// for the pattern to transform.
bool PartialApplyCombiner::combine() {
  // We need to model @unowned_inner_pointer better before we can do the
  // peephole here.
  if (llvm::any_of(pai->getSubstCalleeType()->getResults(),
                   [](SILResultInfo resultInfo) {
                     return resultInfo.getConvention() ==
                            ResultConvention::UnownedInnerPointer;
                   })) {
    return false;
  }

  // Iterate over all uses of the partial_apply
  // and look for applies that use it as a callee.

  // Worklist of operands.
  SmallVector<Operand *, 8> worklist(pai->getUses());
  SmallVector<FullApplySite, 4> foundApplySites;

  while (!worklist.empty()) {
    auto *use = worklist.pop_back_val();
    auto *user = use->getUser();

    // Recurse through copy_value
    if (auto *cvi = dyn_cast<CopyValueInst>(user)) {
      for (auto *copyUse : cvi->getUses())
        worklist.push_back(copyUse);
      continue;
    }

    // Recurse through conversions.
    if (auto *cfi = dyn_cast<ConvertEscapeToNoEscapeInst>(user)) {
      // TODO: Handle argument conversion. All the code in this file needs to be
      // cleaned up and generalized. The argument conversion handling in
      // optimizeApplyOfConvertFunctionInst should apply to any combine
      // involving an apply, not just a specific pattern.
      //
      // For now, just handle conversion to @noescape, which is irrelevant for
      // direct application of the closure.
      auto convertCalleeTy = cfi->getType().castTo<SILFunctionType>();
      auto escapingCalleeTy = convertCalleeTy->getWithExtInfo(
          convertCalleeTy->getExtInfo().withNoEscape(false));
      assert(use->get()->getType().castTo<SILFunctionType>() ==
             escapingCalleeTy);
      (void)escapingCalleeTy;
      llvm::copy(cfi->getUses(), std::back_inserter(worklist));
      continue;
    }

    // Look through mark_dependence users of partial_apply [stack].
    if (auto *mdi = dyn_cast<MarkDependenceInst>(user)) {
      if (mdi->getValue() == use->get() &&
          mdi->getValue()->getType().is<SILFunctionType>() &&
          mdi->getValue()->getType().castTo<SILFunctionType>()->isNoEscape()) {
        llvm::copy(mdi->getUses(), std::back_inserter(worklist));
      }
      continue;
    }
    // If this use of a partial_apply is not
    // an apply which uses it as a callee, bail.
    auto ai = FullApplySite::isa(user);
    if (!ai)
      continue;

    if (ai.getCallee() != use->get())
      continue;

    // We cannot handle generic apply yet. Bail.
    if (ai.hasSubstitutions())
      continue;

    foundApplySites.push_back(ai);
  }

  if (foundApplySites.empty())
    return false;

  if (!copyArgsToTemporaries(foundApplySites))
    return false;

  for (FullApplySite ai : foundApplySites) {
    processSingleApply(ai);
  }

  return true;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

bool swift::tryOptimizeApplyOfPartialApply(PartialApplyInst *pai,
                                           SILBuilderContext &builderCtxt,
                                           InstModCallbacks callbacks) {
  PartialApplyCombiner combiner(pai, builderCtxt, callbacks);
  return combiner.combine();
}
