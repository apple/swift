//===------- LowerHopToExecutor.cpp - Lower hop_to_executor on actors -----===//
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

  /// A map from an actor value to the executor we've derived for it.
  llvm::ScopedHashTable<SILValue, SILValue> ExecutorForActor;

  bool processHop(HopToExecutorInst *hop);
  bool processExtract(ExtractExecutorInst *extract);

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
  bool changed = false;

  auto runOnBlock = [&](SILBasicBlock *block) {
    for (auto ii = block->begin(), ie = block->end(); ii != ie; ) {
      SILInstruction *inst = &*ii++;
      if (auto *hop = dyn_cast<HopToExecutorInst>(inst)) {
        changed |= processHop(hop);
      } else if (auto *extract = dyn_cast<ExtractExecutorInst>(inst)) {
        changed |= processExtract(extract);
      }
    }
  };
  runInDominanceOrderWithScopes(Dominance, runOnBlock, ExecutorForActor);

  return changed;
}

static bool isOptionalBuiltinExecutor(SILType type) {
  if (auto objectType = type.getOptionalObjectType())
    return objectType.is<BuiltinExecutorType>();
  return false;
}

/// Search for hop_to_executor instructions with actor-typed operands.
bool LowerHopToActor::processHop(HopToExecutorInst *hop) {
  auto actor = hop->getTargetExecutor();

  // Ignore hops that are already to Optional<Builtin.Executor>.
  if (isOptionalBuiltinExecutor(actor->getType()))
    return false;

  SILBuilderWithScope B(hop);
  SILValue executor;
  if (actor->getType().is<BuiltinExecutorType>()) {
    // IRGen expects an optional Builtin.Executor, not a Builtin.Executor
    // but we can wrap it nicely
    executor = B.createOptionalSome(
        hop->getLoc(), actor,
        SILType::getOptionalType(actor->getType()));
  } else {
    // Get the dominating executor value for this actor, if available,
    // or else emit code to derive it.
    executor = emitGetExecutor(B, hop->getLoc(), actor, /*optional*/true);
  }
  assert(executor && "executor not set");

  B.createHopToExecutor(hop->getLoc(), executor, /*mandatory*/ false);

  hop->eraseFromParent();

  return true;
}

bool LowerHopToActor::processExtract(ExtractExecutorInst *extract) {
  // Dig out the executor.
  auto executor = extract->getExpectedExecutor();
  if (!isOptionalBuiltinExecutor(executor->getType())) {
    SILBuilderWithScope B(extract);
    executor =
        emitGetExecutor(B, extract->getLoc(), executor, /*optional*/ false);
  }

  // Unconditionally replace the extract with the executor.
  extract->replaceAllUsesWith(executor);
  extract->eraseFromParent();
  return true;
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
  // Get the dominating executor value for this actor, if available,
  // or else emit code to derive it.
  SILValue executor = ExecutorForActor.lookup(actor);
  if (executor) {
    if (makeOptional)
      executor = B.createOptionalSome(loc, executor,
                           SILType::getOptionalType(executor->getType()));
    return executor;
  }

  // This is okay because actor types have to be classes and so never
  // have multiple abstraction patterns.
  CanType actorType = actor->getType().getASTType();

  auto &ctx = F->getASTContext();
  auto resultType = SILType::getPrimitiveObjectType(ctx.TheExecutorType);

  // If the actor type is a default actor, go ahead and devirtualize here.
  auto module = F->getModule().getSwiftModule();
  SILValue unmarkedExecutor;

  // Determine if the actor is a "default actor" in which case we'll build a default
  // actor executor ref inline, rather than calling out to the user-provided executor function.
  if (isDefaultActorType(actorType, module, F->getResilienceExpansion())) {
    auto builtinName = ctx.getIdentifier(
      getBuiltinName(BuiltinValueKind::BuildDefaultActorExecutorRef));
    auto builtinDecl = cast<FuncDecl>(getBuiltinValueDecl(ctx, builtinName));
    auto subs = SubstitutionMap::get(builtinDecl->getGenericSignature(),
                                     {actorType}, {});
    unmarkedExecutor =
      B.createBuiltin(loc, builtinName, resultType, subs, {actor});

    // Otherwise, go through (Distributed)Actor.unownedExecutor.
  } else {
    auto actorKind = actorType->isDistributedActor() ?
                     KnownProtocolKind::DistributedActor :
                     KnownProtocolKind::Actor;
    auto actorProtocol = ctx.getProtocol(actorKind);
    auto req = getUnownedExecutorGetter(ctx, actorProtocol);
    assert(req && "Concurrency library broken");
    SILDeclRef fn(req, SILDeclRef::Kind::Func);

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
    unmarkedExecutor =
      B.createStructExtract(loc, witnessCall, executorProps[0]);
  }

  // Mark the dependence of the resulting value on the actor value to
  // force the actor to stay alive.
  executor = B.createMarkDependence(loc, unmarkedExecutor, actor);

  // Cache the non-optional result for later.
  ExecutorForActor.insert(actor, executor);

  // Inject the result into an optional if requested.
  if (makeOptional)
    executor = B.createOptionalSome(loc, executor,
                           SILType::getOptionalType(executor->getType()));

  return executor;
}

class LowerHopToActorPass : public SILFunctionTransform {

  /// The entry point to the transformation.
  void run() override {
    auto fn = getFunction();
    auto domTree = getAnalysis<DominanceAnalysis>()->get(fn);
    LowerHopToActor pass(getFunction(), domTree);
    if (pass.run())
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
};

} // end anonymous namespace

SILTransform *swift::createLowerHopToActor() {
  return new LowerHopToActorPass();
}
