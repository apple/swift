//===--- SILGenLocalArchetype.cpp - Local archetype transform -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements the transformation which rewrites captured local
//  archetypes into primary archetypes in the enclosing function's generic
//  signature.
//
//===----------------------------------------------------------------------===//

#include "SILGen.h"
#include "swift/AST/LocalArchetypeRequirementCollector.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILCloner.h"

using namespace swift;
using namespace swift::Lowering;

namespace {

class LocalArchetypeTransform : public SILCloner<LocalArchetypeTransform> {
  friend class SILCloner<LocalArchetypeTransform>;
  friend class SILInstructionVisitor<LocalArchetypeTransform>;

  GenericSignatureWithCapturedEnvironments sig;
  GenericEnvironment *env;

public:
  LocalArchetypeTransform(SILFunction *F,
                          GenericSignatureWithCapturedEnvironments sig)
      : SILCloner(*F), env(sig.genericSig.getGenericEnvironment()) {

    assert(!sig.capturedEnvs.empty() && "Why are we doing this?");

    // The primary archetypes of the old generic environment map to
    // primary archetypes of the new generic environment at the same
    // index and depth.
    Functor.SubsMap = env->getForwardingSubstitutionMap();

    // Local archetypes map to generic parameters at higher depths.
    MapLocalArchetypesOutOfContext mapOutOfContext(sig.baseGenericSig,
                                                   sig.capturedEnvs);

    // For each captured environment...
    for (auto *capturedEnv : sig.capturedEnvs) {
      // For each introduced generic parameter...
      auto localParams = capturedEnv->getGenericSignature()
          .getInnermostGenericParams();
      for (auto *gp : localParams) {
        // Get the local archetype from the captured environment.
        auto origArchetypeTy = capturedEnv->mapTypeIntoContext(gp)
            ->castTo<LocalArchetypeType>();

        // Map the local archetype to an interface type in the new generic
        // signature.
        auto substInterfaceTy = mapOutOfContext(origArchetypeTy);

        // Map this interface type into the new generic environment to get
        // a primary archetype.
        auto substArchetypeTy = env->mapTypeIntoContext(substInterfaceTy)
            ->castTo<PrimaryArchetypeType>();

        // Remember this correspondence.
        registerLocalArchetypeRemapping(origArchetypeTy, substArchetypeTy);
      }
    }
  }

  void doIt() {
    auto &F = getBuilder().getFunction();

    // Collect the old basic blocks that we're going to delete.
    llvm::SmallVector<SILBasicBlock *, 4> bbs;
    for (auto &bb : F)
      bbs.push_back(&bb);

    // Make F.mapTypeIntoContext() use the new environment.
    F.setGenericEnvironment(env);

    // Start by cloning the entry block.
    auto *origEntryBlock = F.getEntryBlock();
    auto *clonedEntryBlock = F.createBasicBlock();

    // Clone arguments.
    SmallVector<SILValue, 4> entryArgs;
    entryArgs.reserve(origEntryBlock->getArguments().size());
    for (auto &origArg : origEntryBlock->getArguments()) {

      // Remap the argument type into the new generic environment.
      SILType mappedType = remapType(origArg->getType());
      auto *NewArg = clonedEntryBlock->createFunctionArgument(
          mappedType, origArg->getDecl(), true);
      NewArg->copyFlags(cast<SILFunctionArgument>(origArg));
      entryArgs.push_back(NewArg);
    }

    // Clone the remaining body.
    getBuilder().setInsertionPoint(clonedEntryBlock);
    cloneFunctionBody(&F, clonedEntryBlock, entryArgs,
                      true /*replaceOriginalFunctionInPlace*/);

    // Insert the new entry block at the beginning.
    F.moveBlockBefore(clonedEntryBlock, F.begin());

    // Erase the old basic blocks.
    for (auto *bb : bbs) {
      bb->removeDeadBlock();
    }
  }
};

} // end anonymous namespace

void SILGenModule::recontextualizeCapturedLocalArchetypes(
    SILFunction *F, GenericSignatureWithCapturedEnvironments sig) {
  if (sig.capturedEnvs.empty())
    return;

  LocalArchetypeTransform(F, sig).doIt();
  M.reclaimUnresolvedLocalArchetypeDefinitions();
}
