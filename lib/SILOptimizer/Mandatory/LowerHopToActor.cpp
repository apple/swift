//===------- LowerHopToActor.cpp - Lower hop_to_executor on actors --------===//
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

#define DEBUG_TYPE "insert-hop-to-executor"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/Dominance.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "llvm/ADT/ScopedHashTable.h"

using namespace swift;

namespace {

/// Lower hop_to_executor instructions with actor operands.
///
/// While the language centers actors as the core concept, the runtime
/// is largely expressed in terms of executors, which intentionally are
/// an independent concept.  Every actor has an executor; actors can
/// customize their executor, subject to three restrictions:
///
/// - Any given actor must report the same executor every time its
///   executor is derived.  An actor can be lazy about creating its
///   executor, but it can't have multiple executors, even at different
///   points in its lifetime.
///
/// - Keeping the actor reference alive must keep the executor alive.
///
/// - Derivations of the executor may be freely removed, combined,
///   or sunk by the compiler.  (Whether they should also be hoistable
///   is more contentious.)
///
/// To facilitate full optimization of hops, SILGen emits hops to actors
/// with a hop_to_executor with an actor operand.  (Among other benefits,
/// this means that OptimizeHopToExecutor will eliminate the derivation
/// operation associated with the hop.)  This pass then comes along and
/// inserts the derivations, turning those hops into hops to executors.
/// IRGen expects hops to be to executors before it runs.
class LowerHopToActor {
  SILFunction *F;
  DominanceInfo *Dominance;

  /// A map from an actor value to the dominating instruction that
  /// will derive the executor.
  llvm::ScopedHashTable<SILValue, SILInstruction *>
      ExecutorDerivationForActor;

  /// A multi-map from a dominating {hop_to_|extract_}executor instruction
  /// to other reachable {hop_to_|extract_}executor instructions.
  SmallFrozenMultiMap<SILInstruction *, SILInstruction *, 4>
      DominatingActorHops;

  void recordDominatingInstFor(SILInstruction *inst);
  void rewriteInstructions();

  SILValue emitGetExecutor(SILBuilderWithScope &B,
                           SILLocation loc,
                           SILValue actor, bool makeOptional);

public:
  LowerHopToActor(SILFunction *f,
                  DominanceInfo *dominance)
    : F(f),
      Dominance(dominance)
      { }

  /// The entry point to the transformation.
  bool run();
};

bool LowerHopToActor::run() {
  // Record all actor operands to hop_to_executor and extract_executor
  // and the dominating instruction that will derive the executor.
  auto runOnBlock = [&](SILBasicBlock *block) {
    for (auto ii = block->begin(), ie = block->end(); ii != ie; ) {
      SILInstruction *inst = &*ii++;
      recordDominatingInstFor(inst);
    }
  };
  runInDominanceOrderWithScopes(Dominance, runOnBlock,
                                ExecutorDerivationForActor);

  // If we didn't record any dominating actor hops that need
  // transformation, we're done.
  if (DominatingActorHops.empty())
    return false;

  rewriteInstructions();
  return true;
}

static bool isOptionalBuiltinExecutor(SILType type) {
  if (auto objectType = type.getOptionalObjectType())
    return objectType.is<BuiltinExecutorType>();
  return false;
}

void LowerHopToActor::recordDominatingInstFor(SILInstruction *inst) {
  SILValue actor;
  if (auto *hop = dyn_cast<HopToExecutorInst>(inst)) {
    actor = hop->getTargetExecutor();
    // If hop_to_executor was emitted with an optional executor operand,
    // there's nothing to derive.
    if (isOptionalBuiltinExecutor(actor->getType())) {
      return;
    }
  } else if (auto *extract = dyn_cast<ExtractExecutorInst>(inst)) {
    actor = extract->getExpectedExecutor();
  } else {
    return;
  }

  if (isOptionalBuiltinExecutor(actor->getType()))
    return;

  auto *dominatingInst = ExecutorDerivationForActor.lookup(actor);
  if (dominatingInst) {
    DominatingActorHops.insert(dominatingInst, inst);
  } else {
    DominatingActorHops.insert(inst, inst);
    ExecutorDerivationForActor.insert(actor, inst);
  }

  return;
}

void LowerHopToActor::rewriteInstructions() {
  // Lower the actor operands to executors. Dominating instructions
  // will perform the derivation, and the result will be reused in
  // all reachable instructions.
  DominatingActorHops.setFrozen();
  for (auto domInst : DominatingActorHops.getRange()) {
    auto derivationInst = domInst.first;

    SILValue actor;
    bool makeOptional;
    if (auto *hop = dyn_cast<HopToExecutorInst>(derivationInst)) {
      actor = hop->getTargetExecutor();
      makeOptional = true;
    } else if (auto *extract = dyn_cast<ExtractExecutorInst>(derivationInst)) {
      actor = extract->getExpectedExecutor();
      makeOptional = false;
    } else {
      continue;
    }

    // Emit the executor derivation at the dominating instruction.
    SILBuilderWithScope builder(derivationInst);
    auto executor = emitGetExecutor(
        builder, derivationInst->getLoc(), actor, makeOptional);
    derivationInst->setOperand(0, executor);

    // Set the executor value as the operand for all reachable instructions.
    auto reachableInsts = domInst.second;
    for (auto inst : reachableInsts) {
      if (auto *extract = dyn_cast<ExtractExecutorInst>(inst)) {
        extract->replaceAllUsesWith(executor);
        extract->eraseFromParent();
        continue;
      }

      inst->setOperand(0, executor);
    }
  }
}

static bool isDefaultActorType(CanType actorType, ModuleDecl *M,
                               ResilienceExpansion expansion) {
  if (auto cls = actorType.getClassOrBoundGenericClass())
    return cls->isDefaultActor(M, expansion);
  return false;
}

static AccessorDecl *getUnownedExecutorGetter(ASTContext &ctx,
                                              ProtocolDecl *actorProtocol) {
  for (auto member: actorProtocol->getAllMembers()) {
    if (auto var = dyn_cast<VarDecl>(member)) {
      if (var->getName() == ctx.Id_unownedExecutor)
        return var->getAccessor(AccessorKind::Get);
    }
  }
  return nullptr;
}

SILValue LowerHopToActor::emitGetExecutor(SILBuilderWithScope &B,
                                          SILLocation loc, SILValue actor,
                                          bool makeOptional) {
  // This is okay because actor types have to be classes and so never
  // have multiple abstraction patterns.
  CanType actorType = actor->getType().getASTType();

  // If the operand is already a BuiltinExecutorType, just wrap it
  // in an optional.
  if (makeOptional && actor->getType().is<BuiltinExecutorType>()) {
    return B.createOptionalSome(
        loc, actor,
        SILType::getOptionalType(actor->getType()));
  }

  auto &ctx = F->getASTContext();
  auto executorType = SILType::getPrimitiveObjectType(ctx.TheExecutorType);
  auto optionalExecutorType = SILType::getOptionalType(executorType);

  /// Emit the instructions to derive an executor value from an actor value.
  auto getExecutorFor = [&](SILValue actor) -> SILValue {
    // If the actor type is a default actor, go ahead and devirtualize here.
    auto module = F->getModule().getSwiftModule();
    CanType actorType = actor->getType().getASTType();

    // Determine if the actor is a "default actor" in which case we'll build a default
    // actor executor ref inline, rather than calling out to the user-provided executor function.
    if (isDefaultActorType(actorType, module, F->getResilienceExpansion())) {
      auto builtinName = ctx.getIdentifier(
        getBuiltinName(BuiltinValueKind::BuildDefaultActorExecutorRef));
      auto builtinDecl = cast<FuncDecl>(getBuiltinValueDecl(ctx, builtinName));
      auto subs = SubstitutionMap::get(builtinDecl->getGenericSignature(),
                                       {actorType},
                                       LookUpConformanceInModule(module));
      return B.createBuiltin(loc, builtinName, executorType, subs, {actor});
    }

    // Otherwise, go through (Distributed)Actor.unownedExecutor.
    auto actorKind = actorType->isDistributedActor() ?
                     KnownProtocolKind::DistributedActor :
                     KnownProtocolKind::Actor;
    auto actorProtocol = ctx.getProtocol(actorKind);
    auto req = getUnownedExecutorGetter(ctx, actorProtocol);
    assert(req && "Concurrency library broken");
    SILDeclRef fn(req, SILDeclRef::Kind::Func);

    // Open an existential actor type.
    if (actorType->isExistentialType()) {
      actorType = OpenedArchetypeType::get(
          actorType, F->getGenericSignature())->getCanonicalType();
      SILType loweredActorType = F->getLoweredType(actorType);
      actor = B.createOpenExistentialRef(loc, actor, loweredActorType);
    }

    auto actorConf = module->lookupConformance(actorType, actorProtocol);
    assert(actorConf &&
           "hop_to_executor with actor that doesn't conform to Actor or DistributedActor");

    auto subs = SubstitutionMap::get(req->getGenericSignature(),
                                     {actorType}, {actorConf});
    auto fnType = F->getModule().Types.getConstantFunctionType(*F, fn);

    auto witness =
      B.createWitnessMethod(loc, actorType, actorConf, fn,
                            SILType::getPrimitiveObjectType(fnType));
    auto witnessCall = B.createApply(loc, witness, subs, {actor});

    // The protocol requirement returns an UnownedSerialExecutor; extract
    // the Builtin.Executor from it.
    auto executorDecl = ctx.getUnownedSerialExecutorDecl();
    auto executorProps = executorDecl->getStoredProperties();
    assert(executorProps.size() == 1);
    return B.createStructExtract(loc, witnessCall, executorProps[0]);
  };

  SILValue unmarkedExecutor;
  if (auto wrappedActor = actorType->getOptionalObjectType()) {
    assert(makeOptional);

    // Unwrap the optional and call 'unownedExecutor'.
    auto *someDecl = B.getASTContext().getOptionalSomeDecl();
    auto *curBB = B.getInsertionPoint()->getParent();
    auto *contBB = curBB->split(B.getInsertionPoint());
    auto *someBB = B.getFunction().createBasicBlockAfter(curBB);
    auto *noneBB = B.getFunction().createBasicBlockAfter(someBB);

    unmarkedExecutor = contBB->createPhiArgument(
        optionalExecutorType, actor->getOwnershipKind());

    SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 1> caseBBs;
    caseBBs.push_back(std::make_pair(someDecl, someBB));
    B.setInsertionPoint(curBB);
    auto *switchEnum = B.createSwitchEnum(loc, actor, noneBB, caseBBs);

    SILValue unwrappedActor;
    if (B.hasOwnership()) {
      unwrappedActor = switchEnum->createOptionalSomeResult();
      B.setInsertionPoint(someBB);
    } else {
      B.setInsertionPoint(someBB);
      unwrappedActor = B.createUncheckedEnumData(loc, actor, someDecl);
    }

    // Call 'unownedExecutor' in the some block and wrap the result into
    // an optional.
    SILValue unwrappedExecutor = getExecutorFor(unwrappedActor);
    SILValue someValue =
        B.createOptionalSome(loc, unwrappedExecutor, optionalExecutorType);
    B.createBranch(loc, contBB, {someValue});

    // In the none case, create a nil executor value, which represents
    // the generic executor.
    B.setInsertionPoint(noneBB);
    SILValue noneValue = B.createOptionalNone(loc, optionalExecutorType);
    B.createBranch(loc, contBB, {noneValue});
    B.setInsertionPoint(contBB->begin());
  } else {
    unmarkedExecutor = getExecutorFor(actor);

    // Inject the result into an optional if requested.
    if (makeOptional) {
      unmarkedExecutor = B.createOptionalSome(loc, unmarkedExecutor,
          SILType::getOptionalType(unmarkedExecutor->getType()));
    }
  }

  // Mark the dependence of the resulting value on the actor value to
  // force the actor to stay alive.
  SILValue executor = B.createMarkDependence(loc, unmarkedExecutor, actor,
                                             MarkDependenceKind::Escaping);

  return executor;
}

class LowerHopToActorPass : public SILFunctionTransform {

  /// The entry point to the transformation.
  void run() override {
    auto fn = getFunction();
    auto domTree = getAnalysis<DominanceAnalysis>()->get(fn);
    LowerHopToActor pass(getFunction(), domTree);
    if (pass.run())
      invalidateAnalysis(SILAnalysis::InvalidationKind::BranchesAndInstructions);
  }
};

} // end anonymous namespace

SILTransform *swift::createLowerHopToActor() {
  return new LowerHopToActorPass();
}
