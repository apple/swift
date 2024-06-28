//===--- LLVMARCOpts.cpp - LLVM Reference Counting Optimizations ----------===//
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
//
// This file implements optimizations for reference counting, object allocation,
// and other runtime entrypoints. Most of this code will be removed once the SIL
// level ARC optimizer causes it to no longer be needed.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "swift-llvm-arc-opts"
#include "swift/LLVMPasses/Passes.h"
#include "ARCEntryPointBuilder.h"
#include "LLVMARCOpts.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/NullablePtr.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/APInt.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/Utils/Local.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace swift;
using swift::SwiftARCOpt;

STATISTIC(NumNoopDeleted,
          "Number of no-op swift calls eliminated");
STATISTIC(NumRetainReleasePairs,
          "Number of swift retain/release pairs eliminated");
STATISTIC(NumObjCRetainReleasePairs,
          "Number of objc retain/release pairs eliminated");
STATISTIC(NumAllocateReleasePairs,
          "Number of swift allocate/release pairs eliminated");
STATISTIC(NumStoreOnlyObjectsEliminated,
          "Number of swift stored-only objects eliminated");
STATISTIC(NumUnknownObjectRetainReleaseSRed,
          "Number of unknownretain/release strength reduced to retain/release");

llvm::cl::opt<bool>
DisableARCOpts("disable-llvm-arc-opts", llvm::cl::init(false));

//===----------------------------------------------------------------------===//
//                          Input Function Canonicalizer
//===----------------------------------------------------------------------===//

/// canonicalizeInputFunction - Functions like swift_retain return an
/// argument as a low-level performance optimization.  This makes it difficult
/// to reason about pointer equality though, so undo it as an initial
/// canonicalization step.  After this step, all swift_retain's have been
/// replaced with swift_retain.
///
/// This also does some trivial peep-hole optimizations as we go.
static bool canonicalizeInputFunction(Function &F, ARCEntryPointBuilder &B,
                                      SwiftRCIdentity &RC) {
  bool Changed = false;
  DenseSet<Value *> NativeRefs;
  DenseMap<Value *, TinyPtrVector<Instruction *>> UnknownObjectRetains;
  DenseMap<Value *, TinyPtrVector<Instruction *>> UnknownObjectReleases;
  for (auto &BB : F) {
    UnknownObjectRetains.clear();
    UnknownObjectReleases.clear();
    NativeRefs.clear();
    for (auto I = BB.begin(); I != BB.end(); ) {
      Instruction &Inst = *I++;

      switch (classifyInstruction(Inst)) {
      // These instructions should not reach here based on the pass ordering.
      // i.e. LLVMARCOpt -> LLVMContractOpt.
      case RT_RetainN:
      case RT_UnknownObjectRetainN:
      case RT_BridgeRetainN:
      case RT_ReleaseN:
      case RT_UnknownObjectReleaseN:
      case RT_BridgeReleaseN:
        llvm_unreachable("These are only created by LLVMARCContract !");
      case RT_Unknown:
      case RT_BridgeRelease:
      case RT_AllocObject:
      case RT_FixLifetime:
      case RT_EndBorrow:
      case RT_NoMemoryAccessed:
      case RT_RetainUnowned:
      case RT_CheckUnowned:
        break;
      case RT_Retain: {
        CallInst &CI = cast<CallInst>(Inst);
        Value *ArgVal = RC.getSwiftRCIdentityRoot(CI.getArgOperand(0));
        // retain(null) is a no-op.
        if (isa<ConstantPointerNull>(ArgVal)) {
          CI.eraseFromParent();
          Changed = true;
          ++NumNoopDeleted;
          continue;
        }
        if (!CI.use_empty()) {
          // Do not get RC identical value here, could end up with a
          // crash in replaceAllUsesWith as the type maybe different.
          CI.replaceAllUsesWith(CI.getArgOperand(0));
          Changed = true;
        }
        // Rewrite unknown retains into swift_retains.
        NativeRefs.insert(ArgVal);
        for (auto &X : UnknownObjectRetains[ArgVal]) {
          B.setInsertPoint(X);
          B.createRetain(ArgVal, cast<CallInst>(X));
          X->eraseFromParent();
          ++NumUnknownObjectRetainReleaseSRed;
          Changed = true;
        }
        UnknownObjectRetains[ArgVal].clear();
        break;
      }
      case RT_UnknownObjectRetain: {
        CallInst &CI = cast<CallInst>(Inst);
        Value *ArgVal = RC.getSwiftRCIdentityRoot(CI.getArgOperand(0));
        // unknownObjectRetain(null) is a no-op.
        if (isa<ConstantPointerNull>(ArgVal)) {
          CI.eraseFromParent();
          Changed = true;
          ++NumNoopDeleted;
          continue;
        }
        if (!CI.use_empty()) {
          // Do not get RC identical value here, could end up with a
          // crash in replaceAllUsesWith as the type maybe different.
          CI.replaceAllUsesWith(CI.getArgOperand(0));
          Changed = true;
        }
        // Have not encountered a strong retain/release. keep it in the
        // unknown retain/release list for now. It might get replaced
        // later.
        if (!NativeRefs.contains(ArgVal)) {
          UnknownObjectRetains[ArgVal].push_back(&CI);
        } else {
          B.setInsertPoint(&CI);
          B.createRetain(ArgVal, &CI);
          CI.eraseFromParent();
          ++NumUnknownObjectRetainReleaseSRed;
          Changed = true;
        }
        break;
      }
      case RT_Release: {
        CallInst &CI = cast<CallInst>(Inst);
        Value *ArgVal = RC.getSwiftRCIdentityRoot(CI.getArgOperand(0));
        // release(null) is a no-op.
        if (isa<ConstantPointerNull>(ArgVal)) {
          CI.eraseFromParent();
          Changed = true;
          ++NumNoopDeleted;
          continue;
        }
        // Rewrite unknown releases into swift_releases.
        NativeRefs.insert(ArgVal);
        for (auto &X : UnknownObjectReleases[ArgVal]) {
          B.setInsertPoint(X);
          B.createRelease(ArgVal, cast<CallInst>(X));
          X->eraseFromParent();
          ++NumUnknownObjectRetainReleaseSRed;
          Changed = true;
        }
        UnknownObjectReleases[ArgVal].clear();
        break;
      }
      case RT_UnknownObjectRelease: {
        CallInst &CI = cast<CallInst>(Inst);
        Value *ArgVal = RC.getSwiftRCIdentityRoot(CI.getArgOperand(0));
        // unknownObjectRelease(null) is a no-op.
        if (isa<ConstantPointerNull>(ArgVal)) {
          CI.eraseFromParent();
          Changed = true;
          ++NumNoopDeleted;
          continue;
        }

        // Have not encountered a strong retain/release. keep it in the
        // unknown retain/release list for now. It might get replaced
        // later.
        if (!NativeRefs.contains(ArgVal)) {
          UnknownObjectReleases[ArgVal].push_back(&CI);
        } else {
          B.setInsertPoint(&CI);
          B.createRelease(ArgVal, &CI);
          CI.eraseFromParent();
          ++NumUnknownObjectRetainReleaseSRed;
          Changed = true;
        }
        break;
      }
      case RT_ObjCRelease: {
        CallInst &CI = cast<CallInst>(Inst);
        Value *ArgVal = RC.getSwiftRCIdentityRoot(CI.getArgOperand(0));
        // objc_release(null) is a noop, zap it.
        if (isa<ConstantPointerNull>(ArgVal)) {
          CI.eraseFromParent();
          Changed = true;
          ++NumNoopDeleted;
          continue;
        }
        break;
      }

      // These retain instructions return their argument so must be processed
      // specially.
      case RT_BridgeRetain:
      case RT_ObjCRetain: {
        // Canonicalize the retain so that nothing uses its result.
        CallInst &CI = cast<CallInst>(Inst);
        // Do not get RC identical value here, could end up with a
        // crash in replaceAllUsesWith as the type maybe different.
        Value *ArgVal = CI.getArgOperand(0);
        if (!CI.use_empty()) {
          CI.replaceAllUsesWith(ArgVal);
          Changed = true;
        }

        // {objc_retain,swift_unknownObjectRetain}(null) is a noop, delete it.
        if (isa<ConstantPointerNull>(ArgVal)) {
          CI.eraseFromParent();
          Changed = true;
          ++NumNoopDeleted;
          continue;
        }

        break;
      }
      }
    }
  }
  return Changed;
}

//===----------------------------------------------------------------------===//
//                         Release() Motion
//===----------------------------------------------------------------------===//

/// performLocalReleaseMotion - Scan backwards from the specified release,
/// moving it earlier in the function if possible, over instructions that do not
/// access the released object.  If we get to a retain or allocation of the
/// object, zap both.
static bool performLocalReleaseMotion(CallInst &Release, BasicBlock &BB,
                                      SwiftRCIdentity &RC) {
  // FIXME: Call classifier should identify the object for us.  Too bad C++
  // doesn't have nice Swift-style enums.
  Value *ReleasedObject = RC.getSwiftRCIdentityRoot(Release.getArgOperand(0));

  BasicBlock::iterator BBI = Release.getIterator();

  // Scan until we get to the top of the block.
  while (BBI != BB.begin()) {
    --BBI;

    // Don't analyze PHI nodes and landingpad instructions. We can't move
    // releases before them and they aren't "interesting".
    if (isa<PHINode>(BBI) || isa<LandingPadInst>(BBI) ||
        // If we found the instruction that defines the value we're releasing,
        // don't push the release past it.
        &*BBI == Release.getArgOperand(0)) {
      ++BBI;
      goto OutOfLoop;
    }

    switch (classifyInstruction(*BBI)) {
    // These instructions should not reach here based on the pass ordering.
    // i.e. LLVMARCOpt -> LLVMContractOpt.
    case RT_UnknownObjectRetainN:
    case RT_BridgeRetainN:
    case RT_RetainN:
    case RT_UnknownObjectReleaseN:
    case RT_BridgeReleaseN:
    case RT_ReleaseN:
        llvm_unreachable("These are only created by LLVMARCContract !");
    case RT_NoMemoryAccessed:
      // Skip over random instructions that don't touch memory.  They don't need
      // protection by retain/release.
      continue;

    case RT_UnknownObjectRelease:
    case RT_BridgeRelease:
    case RT_ObjCRelease:
    case RT_Release: {
      // If we get to a release, we can generally ignore it and scan past it.
      // However, if we get to a release of obviously the same object, we stop
      // scanning here because it should have already be moved as early as
      // possible, so there is no reason to move its friend to the same place.
      //
      // NOTE: If this occurs frequently, maybe we can have a release(Obj, N)
      // API to drop multiple retain counts at once.
      CallInst &ThisRelease = cast<CallInst>(*BBI);
      Value *ThisReleasedObject = ThisRelease.getArgOperand(0);
      ThisReleasedObject = RC.getSwiftRCIdentityRoot(ThisReleasedObject);
      if (ThisReleasedObject == ReleasedObject) {
        //Release.dump(); ThisRelease.dump(); BB.getParent()->dump();
        ++BBI;
        goto OutOfLoop;
      }
      continue;
    }

    case RT_UnknownObjectRetain:
    case RT_BridgeRetain:
    case RT_ObjCRetain:
    case RT_Retain: {  // swift_retain(obj)
      CallInst &Retain = cast<CallInst>(*BBI);
      Value *RetainedObject = Retain.getArgOperand(0);
      RetainedObject = RC.getSwiftRCIdentityRoot(RetainedObject);

      // Since we canonicalized earlier, we know that if our retain has any
      // uses, they were replaced already. This assertion documents this
      // assumption.
      assert(Retain.use_empty() && "Retain should have been canonicalized to "
             "have no uses.");

      // If the retain and release are to obviously pointer-equal objects, then
      // we can delete both of them.  We have proven that they do not protect
      // anything of value.
      if (RetainedObject == ReleasedObject) {
        Retain.eraseFromParent();
        Release.eraseFromParent();
        ++NumRetainReleasePairs;
        return true;
      }

      // Otherwise, this is a retain of an object that is not statically known
      // to be the same object.  It may still be dynamically the same object
      // though.  In this case, we can't move the release past it.
      // TODO: Strengthen analysis.
      //Release.dump(); ThisRelease.dump(); BB.getParent()->dump();
     ++BBI;
      goto OutOfLoop;
    }

    case RT_AllocObject: {   // %obj = swift_alloc(...)
      CallInst &Allocation = cast<CallInst>(*BBI);

      // If this is an allocation of an unrelated object, just ignore it.
      // TODO: This is not safe without proving the object being released is not
      // related to the allocated object.  Consider something silly like this:
      //   A = allocate()
      //   B = bitcast A to object
      //   release(B)
      if (ReleasedObject != &Allocation) {
        // Release.dump(); BB.getParent()->dump();
        ++BBI;
        goto OutOfLoop;
      }

      // If this is a release right after an allocation of the object, then we
      // can zap both.
      Allocation.replaceAllUsesWith(UndefValue::get(Allocation.getType()));
      Allocation.eraseFromParent();
      Release.eraseFromParent();
      ++NumAllocateReleasePairs;
      return true;
    }

    case RT_FixLifetime:
    case RT_EndBorrow:
    case RT_RetainUnowned:
    case RT_CheckUnowned:
    case RT_Unknown:
      // Otherwise, we have reached something that we do not understand. Do not
      // attempt to shorten the lifetime of this object beyond this point so we
      // are conservative.
      ++BBI;
      goto OutOfLoop;
    }
  }
OutOfLoop:


  // If we got to the top of the block, (and if the instruction didn't start
  // there) move the release to the top of the block.
  // TODO: This is where we'd plug in some global algorithms someday.
  if (&*BBI != &Release) {
    Release.moveBefore(&*BBI);
    return true;
  }

  return false;
}

//===----------------------------------------------------------------------===//
//                         Retain() Motion
//===----------------------------------------------------------------------===//

/// performLocalRetainMotion - Scan forward from the specified retain, moving it
/// later in the function if possible, over instructions that provably can't
/// release the object.  If we get to a release of the object, zap both.
///
/// NOTE: this handles both objc_retain and swift_retain.
///
static bool performLocalRetainMotion(CallInst &Retain, BasicBlock &BB,
                                     SwiftRCIdentity &RC) {
  // FIXME: Call classifier should identify the object for us.  Too bad C++
  // doesn't have nice Swift-style enums.
  Value *RetainedObject = RC.getSwiftRCIdentityRoot(Retain.getArgOperand(0));

  BasicBlock::iterator BBI = Retain.getIterator(),
                       BBE = BB.getTerminator()->getIterator();

  bool isObjCRetain = Retain.getIntrinsicID() == llvm::Intrinsic::objc_retain;

  bool MadeProgress = false;

  // Scan until we get to the end of the block.
  for (++BBI; BBI != BBE; ++BBI) {
    Instruction &CurInst = *BBI;

    // Classify the instruction. This switch does a "break" when the instruction
    // can be skipped and is interesting, and a "continue" when it is a retain
    // of the same pointer.
    switch (classifyInstruction(CurInst)) {
    // These instructions should not reach here based on the pass ordering.
    // i.e. LLVMARCOpt -> LLVMContractOpt.
    case RT_RetainN:
    case RT_UnknownObjectRetainN:
    case RT_BridgeRetainN:
    case RT_ReleaseN:
    case RT_UnknownObjectReleaseN:
    case RT_BridgeReleaseN:
        llvm_unreachable("These are only created by LLVMARCContract !");
    case RT_NoMemoryAccessed:
    case RT_AllocObject:
    case RT_CheckUnowned:
      // Skip over random instructions that don't touch memory.  They don't need
      // protection by retain/release.
      break;

    case RT_FixLifetime: // This only stops release motion. Retains can move over it.
    case RT_EndBorrow:
      break;

    case RT_Retain:
    case RT_UnknownObjectRetain:
    case RT_BridgeRetain:
    case RT_RetainUnowned:
    case RT_ObjCRetain: {  // swift_retain(obj)
      //CallInst &ThisRetain = cast<CallInst>(CurInst);
      //Value *ThisRetainedObject = ThisRetain.getArgOperand(0);

      // If we see a retain of the same object, we can skip over it, but we
      // can't count it as progress.  Just pushing a retain(x) past a retain(y)
      // doesn't change the program.
      continue;
    }


    case RT_UnknownObjectRelease:
    case RT_BridgeRelease:
    case RT_ObjCRelease:
    case RT_Release: {
      // If we get to a release that is provably to this object, then we can zap
      // it and the retain.
      CallInst &ThisRelease = cast<CallInst>(CurInst);
      Value *ThisReleasedObject = ThisRelease.getArgOperand(0);
      ThisReleasedObject = RC.getSwiftRCIdentityRoot(ThisReleasedObject);
      if (ThisReleasedObject == RetainedObject) {
        Retain.eraseFromParent();
        ThisRelease.eraseFromParent();
        if (isObjCRetain) {
          ++NumObjCRetainReleasePairs;
        } else {
          ++NumRetainReleasePairs;
        }
        return true;
      }

      // Otherwise, if this is some other pointer, we can only ignore it if we
      // can prove that the two objects don't alias.
      // Retain.dump(); ThisRelease.dump(); BB.getParent()->dump();
      goto OutOfLoop;
    }

    case RT_Unknown:
      // Loads cannot affect the retain.
      if (isa<LoadInst>(CurInst))
        continue;

      // Load, store, memcpy etc can't do a release.
      if (isa<LoadInst>(CurInst) || isa<StoreInst>(CurInst) ||
          isa<MemIntrinsic>(CurInst))
        break;

      // CurInst->dump(); BBI->dump();
      // Otherwise, we get to something unknown/unhandled.  Bail out for now.
      goto OutOfLoop;
    }

    // If the switch did a break, we made some progress moving this retain.
    MadeProgress = true;
  }
OutOfLoop:

  // If we were able to move the retain down, move it now.
  // TODO: This is where we'd plug in some global algorithms someday.
  if (MadeProgress) {
    Retain.moveBefore(&*BBI);
    return true;
  }

  return false;
}

//===----------------------------------------------------------------------===//
//                       Store-Only Object Elimination
//===----------------------------------------------------------------------===//

/// DT_Kind - Classification for destructor semantics.
enum class DtorKind {
  /// NoSideEffects - The destructor does nothing, or just touches the local
  /// object in a non-observable way after it is destroyed.
  NoSideEffects,

  /// NoEscape - The destructor potentially has some side effects, but the
  /// address of the destroyed object never escapes (in the LLVM IR sense).
  NoEscape,

  /// Unknown - Something potentially crazy is going on here.
  Unknown
};

/// analyzeDestructor - Given the heap.metadata argument to swift_allocObject,
/// take a look a the destructor and try to decide if it has side effects or any
/// other bad effects that can prevent it from being optimized.
static DtorKind analyzeDestructor(Value *P) {
  // If we have a null pointer for the metadata info, the dtor has no side
  // effects.  Actually, the final release would crash.  This is really only
  // useful for writing testcases.
  if (isa<ConstantPointerNull>(P->stripPointerCasts()))
    return DtorKind::NoSideEffects;

  // We have to have a known heap metadata value, reject dynamically computed
  // ones, or places
  // Also, make sure we have a definitive initializer for the global.
  auto *GV = dyn_cast<GlobalVariable>(P->stripPointerCasts());
  if (GV == nullptr || !GV->hasDefinitiveInitializer())
    return DtorKind::Unknown;

  ConstantStruct *CS = dyn_cast_or_null<ConstantStruct>(GV->getInitializer());
  if (CS == nullptr || CS->getNumOperands() == 0)
    return DtorKind::Unknown;

  // FIXME: Would like to abstract the dtor slot (#0) out from this to somewhere
  // unified.
  enum { DTorSlotOfHeapMetadata = 0 };
  auto *DtorFn = dyn_cast<Function>(CS->getOperand(DTorSlotOfHeapMetadata));
  if (DtorFn == nullptr || DtorFn->isInterposable() ||
      DtorFn->hasExternalLinkage())
    return DtorKind::Unknown;

  // Okay, we have a body, and we can trust it.  If the function is marked
  // readonly, then we know it can't have any interesting side effects, so we
  // don't need to analyze it at all.
  if (DtorFn->onlyReadsMemory())
    return DtorKind::NoSideEffects;

  // The first argument is the object being destroyed.
  assert(DtorFn->arg_size() == 1 && !DtorFn->isVarArg() &&
         "expected a single object argument to destructors");
  Value *ThisObject = &*DtorFn->arg_begin();

  // Scan the body of the function, looking for anything scary.
  for (BasicBlock &BB : *DtorFn) {
    for (Instruction &I : BB) {
      // Note that the destructor may not be in any particular canonical form.
      switch (classifyInstruction(I)) {
      // These instructions should not reach here based on the pass ordering.
      // i.e. LLVMARCOpt -> LLVMContractOpt.
      case RT_RetainN:
      case RT_UnknownObjectRetainN:
      case RT_BridgeRetainN:
      case RT_ReleaseN:
      case RT_UnknownObjectReleaseN:
      case RT_BridgeReleaseN:
        llvm_unreachable("These are only created by LLVMARCContract !");
      case RT_NoMemoryAccessed:
      case RT_AllocObject:
      case RT_FixLifetime:
      case RT_EndBorrow:
      case RT_CheckUnowned:
        // Skip over random instructions that don't touch memory in the caller.
        continue;

      case RT_RetainUnowned:
      case RT_BridgeRetain:          // x = swift_bridgeRetain(y)
      case RT_Retain: {      // swift_retain(obj)

        // Ignore retains of the "self" object, no resurrection is possible.
        Value *ThisRetainedObject = cast<CallInst>(I).getArgOperand(0);
        if (ThisRetainedObject->stripPointerCasts() ==
            ThisObject->stripPointerCasts())
          continue;
        // Otherwise, we may be retaining something scary.
        break;
      }

      case RT_Release: {
        // If we get to a release that is provably to this object, then we can
        // ignore it.
        Value *ThisReleasedObject = cast<CallInst>(I).getArgOperand(0);

        if (ThisReleasedObject->stripPointerCasts() ==
            ThisObject->stripPointerCasts())
          continue;
        // Otherwise, we may be retaining something scary.
        break;
      }

      case RT_ObjCRelease:
      case RT_ObjCRetain:
      case RT_UnknownObjectRetain:
      case RT_UnknownObjectRelease:
      case RT_BridgeRelease:
        // Objective-C retain and release can have arbitrary side effects.
        break;

      case RT_Unknown:
        // Ignore all instructions with no side effects.
        if (!I.mayHaveSideEffects()) continue;

        // store, memcpy, memmove *to* the object can be dropped.
        if (auto *SI = dyn_cast<StoreInst>(&I)) {
          if (SI->getPointerOperand()->stripInBoundsOffsets() == ThisObject)
            continue;
        }

        if (auto *MI = dyn_cast<MemIntrinsic>(&I)) {
          if (MI->getDest()->stripInBoundsOffsets() == ThisObject)
            continue;
        }

        // Otherwise, we can't remove the deallocation completely.
        break;
      }

      // Okay, the function has some side effects.
      //
      // TODO: We could in the future return more accurate information by
      // checking if the function is able to capture the deinit parameter. We do
      // not do that today.
      return DtorKind::Unknown;
    }
  }

  // If we didn't find any side effects, we win.
  return DtorKind::NoSideEffects;
}


/// performStoreOnlyObjectElimination - Scan the graph of uses of the specified
/// object allocation.  If the object does not escape and is only stored to
/// (this happens because GVN and other optimizations hoists forward substitutes
/// all stores to the object to eliminate all loads from it), then zap the
/// object and all accesses related to it.
static bool performStoreOnlyObjectElimination(CallInst &Allocation,
                                              BasicBlock::iterator &BBI) {
  DtorKind DtorInfo = analyzeDestructor(Allocation.getArgOperand(0));

  // We can't delete the object if its destructor has side effects.
  if (DtorInfo != DtorKind::NoSideEffects)
    return false;

  // Do a depth first search exploring all of the uses of the object pointer,
  // following through casts, pointer adjustments etc.  If we find any loads or
  // any escape sites of the object, we give up.  If we succeed in walking the
  // entire graph of uses, we can remove the resultant set.
  SmallSetVector<Instruction*, 16> InvolvedInstructions;
  SmallVector<Instruction*, 16> Worklist;
  Worklist.push_back(&Allocation);

  // Stores - Keep track of all of the store instructions we see.
  SmallVector<StoreInst*, 16> Stores;

  while (!Worklist.empty()) {
    Instruction *I = Worklist.pop_back_val();

    // Insert the instruction into our InvolvedInstructions set.  If we have
    // already seen it, then don't reprocess all of the uses.
    if (!InvolvedInstructions.insert(I)) continue;

    // Okay, this is the first time we've seen this instruction, proceed.
    switch (classifyInstruction(*I)) {
    // These instructions should not reach here based on the pass ordering.
    // i.e. LLVMARCOpt -> LLVMContractOpt.
    case RT_RetainN:
    case RT_UnknownObjectRetainN:
    case RT_BridgeRetainN:
    case RT_ReleaseN:
    case RT_UnknownObjectReleaseN:
    case RT_BridgeReleaseN:
      llvm_unreachable("These are only created by LLVMARCContract !");
    case RT_AllocObject:
      // If this is a different swift_allocObject than we started with, then
      // there is some computation feeding into a size or alignment computation
      // that we have to keep... unless we can delete *that* entire object as
      // well.
      break;

    case RT_NoMemoryAccessed:
      // If no memory is accessed, then something is being done with the
      // pointer: maybe it is bitcast or GEP'd. Since there are no side effects,
      // it is perfectly fine to delete this instruction if all uses of the
      // instruction are also eliminable.

      if (I->mayHaveSideEffects() || I->isTerminator())
        return false;
      break;

    case RT_Release:
    case RT_Retain:
    case RT_FixLifetime:
    case RT_EndBorrow:
    case RT_CheckUnowned:
      // It is perfectly fine to eliminate various retains and releases of this
      // object: we are zapping all accesses or none.
      break;

    // If this is an unknown instruction, we have more interesting things to
    // consider.
    case RT_Unknown:
    case RT_ObjCRelease:
    case RT_ObjCRetain:
    case RT_UnknownObjectRetain:
    case RT_UnknownObjectRelease:
    case RT_BridgeRetain:
    case RT_BridgeRelease:
    case RT_RetainUnowned:

      // Otherwise, this really is some unhandled instruction.  Bail out.
      return false;
    }

    // Okay, if we got here, the instruction can be eaten so-long as all of its
    // uses can be.  Scan through the uses and add them to the worklist for
    // recursive processing.
    for (auto UI = I->user_begin(), E = I->user_end(); UI != E; ++UI) {
      Instruction *User = cast<Instruction>(*UI);

      // Handle stores as a special case here: we want to make sure that the
      // object is being stored *to*, not itself being stored (which would be an
      // escape point).  Since stores themselves don't have any uses, we can
      // short-cut the classification scheme above.
      if (auto *SI = dyn_cast<StoreInst>(User)) {
        // If this is a store *to* the object, we can zap it.
        if (UI.getUse().getOperandNo() == StoreInst::getPointerOperandIndex()) {
          InvolvedInstructions.insert(SI);
          continue;
        }
        // Otherwise, using the object as a source (or size) is an escape.
        return false;
      }
      if (auto *MI = dyn_cast<MemIntrinsic>(User)) {
        // If this is a memset/memcpy/memmove *to* the object, we can zap it.
        if (UI.getUse().getOperandNo() == 0) {
          InvolvedInstructions.insert(MI);
          continue;
        }
        // Otherwise, using the object as a source (or size) is an escape.
        return false;
      }

      // Otherwise, normal instructions just go on the worklist for processing.
      Worklist.push_back(User);
    }
  }

  // Ok, we succeeded!  This means we can zap all of the instructions that use
  // the object.  One thing we have to be careful of is to make sure that we
  // don't invalidate "BBI" (the iterator the outer walk of the optimization
  // pass is using, and indicates the next instruction to process).  This would
  // happen if we delete the instruction it is pointing to.  Advance the
  // iterator if that would happen.
  while (InvolvedInstructions.count(&*BBI))
    ++BBI;

  // Zap all of the instructions.
  for (auto I : InvolvedInstructions) {
    if (!I->use_empty())
      I->replaceAllUsesWith(UndefValue::get(I->getType()));
    I->eraseFromParent();
  }

  ++NumStoreOnlyObjectsEliminated;
  return true;
}

/// Gets the underlying address of a load.
static Value *getBaseAddress(Value *val) {
  for (;;) {
    if (auto *GEP = dyn_cast<GetElementPtrInst>(val)) {
      val = GEP->getPointerOperand();
      continue;
    }
    if (auto *BC = dyn_cast<BitCastInst>(val)) {
      val = BC->getOperand(0);
      continue;
    }
    return val;
  }
}

/// Replaces
///
///   strong_retain_unowned %x
///   ... // speculatively executable instructions, including loads from %x
///   strong_release %x
///
/// with
///
///   ... // speculatively executable instructions, including loads from %x
///   check_unowned %x
///
static bool performLocalRetainUnownedOpt(CallInst *Retain, BasicBlock &BB,
                                         ARCEntryPointBuilder &B) {
  Value *RetainedObject = Retain->getArgOperand(0);
  Value *LoadBaseAddr = getBaseAddress(RetainedObject);

  auto BBI = Retain->getIterator(), BBE = BB.getTerminator()->getIterator();

  // Scan until we get to the end of the block.
  for (++BBI; BBI != BBE; ++BBI) {
    Instruction &I = *BBI;
    
    if (classifyInstruction(I) == RT_Release) {
      CallInst *ThisRelease = cast<CallInst>(&I);
      
      // Is this the trailing release of the unowned-retained reference?
      if (ThisRelease->getArgOperand(0) != RetainedObject)
        return false;
      
      // Replace the trailing release with a check_unowned.
      B.setInsertPoint(ThisRelease);
      B.createCheckUnowned(RetainedObject, ThisRelease);
      Retain->eraseFromParent();
      ThisRelease->eraseFromParent();
      ++NumRetainReleasePairs;
      return true;
    }
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      // Accept loads from the unowned-referenced object. This may load garbage
      // values, but they are not used until the final check_unowned succeeds.
      if (getBaseAddress(LI->getPointerOperand()) == LoadBaseAddr)
        continue;
    }
    // Other than loads from the unowned-referenced object we only accept
    // speculatively executable instructions.
    if (!isSafeToSpeculativelyExecute(&I))
      return false;
  }
  return false;
}

/// Removes redundant check_unowned calls if they check the same reference and
/// there is no instruction in between which could decrement the reference count.
static void performRedundantCheckUnownedRemoval(BasicBlock &BB) {
  DenseSet<Value *> checkedValues;
  for (BasicBlock::iterator BBI = BB.begin(), E = BB.end(); BBI != E; ) {
    // Preincrement the iterator to avoid invalidation and out trouble.
    Instruction &I = *BBI++;
    switch (classifyInstruction(I)) {
      case RT_NoMemoryAccessed:
      case RT_AllocObject:
      case RT_FixLifetime:
      case RT_Retain:
      case RT_UnknownObjectRetain:
      case RT_BridgeRetain:
      case RT_RetainUnowned:
      case RT_ObjCRetain:
        // All this cannot decrement reference counts.
        continue;

      case RT_CheckUnowned: {
        Value *Arg = cast<CallInst>(&I)->getArgOperand(0);
        if (checkedValues.count(Arg) != 0) {
          // We checked this reference already -> delete the second check.
          I.eraseFromParent();
        } else {
          // Record the check.
          checkedValues.insert(Arg);
        }
        continue;
      }
        
      case RT_Unknown:
        // Loads cannot affect the retain.
        if (isa<LoadInst>(I) || isa<StoreInst>(I) || isa<MemIntrinsic>(I))
          continue;
        break;
        
      default:
        break;
    }
    // We found some potential reference decrementing instruction. Bail out.
    checkedValues.clear();
  }
}

/// performGeneralOptimizations - This does a forward scan over basic blocks,
/// looking for interesting local optimizations that can be done.
static bool performGeneralOptimizations(Function &F, ARCEntryPointBuilder &B,
                                        SwiftRCIdentity &RC) {
  bool Changed = false;

  // TODO: This is a really trivial local algorithm.  It could be much better.
  for (BasicBlock &BB : F) {
    SmallVector<CallInst *, 8> RetainUnownedInsts;
    
    for (BasicBlock::iterator BBI = BB.begin(), E = BB.end(); BBI != E; ) {
      // Preincrement the iterator to avoid invalidation and out trouble.
      Instruction &I = *BBI++;

      // Do various optimizations based on the instruction we find.
      switch (classifyInstruction(I)) {
      default: break;
      case RT_AllocObject:
        Changed |= performStoreOnlyObjectElimination(cast<CallInst>(I), BBI);
        break;
      case RT_BridgeRelease:
      case RT_ObjCRelease:
      case RT_UnknownObjectRelease:
      case RT_Release:
        Changed |= performLocalReleaseMotion(cast<CallInst>(I), BB, RC);
        break;
      case RT_BridgeRetain:
      case RT_Retain:
      case RT_UnknownObjectRetain:
      case RT_ObjCRetain: {
        // Retain motion is a forward pass over the block.  Make sure we don't
        // invalidate our iterators by parking it on the instruction before I.
        BasicBlock::iterator Safe = I.getIterator();
        Safe = Safe != BB.begin() ? std::prev(Safe) : BB.end();
        if (performLocalRetainMotion(cast<CallInst>(I), BB, RC)) {
          // If we zapped or moved the retain, reset the iterator on the
          // instruction *newly* after the prev instruction.
          BBI = Safe != BB.end() ? std::next(Safe) : BB.begin();
          Changed = true;
        }
        break;
      }
      case RT_RetainUnowned:
        RetainUnownedInsts.push_back(cast<CallInst>(&I));
        break;
      }
    }
    // Delay the retain-unowned optimization until we finished with all other
    // optimizations in this block. The retain-unowned optimization will benefit
    // from the release-motion.
    bool CheckUnknownInserted = false;
    for (auto *RetainUnowned : RetainUnownedInsts) {
      if (performLocalRetainUnownedOpt(RetainUnowned, BB, B))
        CheckUnknownInserted = true;
    }
    if (CheckUnknownInserted) {
      Changed = true;
      performRedundantCheckUnownedRemoval(BB);
    }
  }
  return Changed;
}

//===----------------------------------------------------------------------===//
//                            SwiftARCOpt Pass
//===----------------------------------------------------------------------===//

char SwiftARCOpt::ID = 0;

INITIALIZE_PASS_BEGIN(SwiftARCOpt,
                      "swift-llvm-arc-optimize", "Swift LLVM ARC optimization",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(SwiftAAWrapperPass)
INITIALIZE_PASS_END(SwiftARCOpt,
                    "swift-llvm-arc-optimize", "Swift LLVM ARC optimization",
                    false, false)

// Optimization passes.
llvm::FunctionPass *swift::createSwiftARCOptPass() {
  initializeSwiftARCOptPass(*llvm::PassRegistry::getPassRegistry());
  return new SwiftARCOpt();
}

SwiftARCOpt::SwiftARCOpt() : FunctionPass(ID) {
}


void SwiftARCOpt::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.addRequiredID(&SwiftAAWrapperPass::ID);
  AU.setPreservesCFG();
}

static bool runSwiftARCOpts(Function &F, SwiftRCIdentity &RC) {
  bool Changed = false;
  ARCEntryPointBuilder B(F);

  // First thing: canonicalize swift_retain and similar calls so that nothing
  // uses their result.  This exposes the copy that the function does to the
  // optimizer.
  Changed |= canonicalizeInputFunction(F, B, RC);

  // Next, do a pass with a couple of optimizations:
  // 1) release() motion, eliminating retain/release pairs when it turns out
  //    that a pair is not protecting anything that accesses the guarded heap
  //    object.
  // 2) deletion of stored-only objects - objects that are allocated and
  //    potentially retained and released, but are only stored to and don't
  //    escape.
  Changed |= performGeneralOptimizations(F, B, RC);

  return Changed;
}

bool SwiftARCOpt::runOnFunction(Function &F) {
  if (DisableARCOpts)
    return false;

  return runSwiftARCOpts(F, RC);
}

PreservedAnalyses SwiftARCOptPass::run(llvm::Function &F,
                                       llvm::FunctionAnalysisManager &AM) {
  bool changed = false;

  if (!DisableARCOpts)
    changed = runSwiftARCOpts(F, RC);

  if (!changed) {
    return PreservedAnalyses::all();
  }

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
