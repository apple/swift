//===--- SILCombiner.h ------------------------------------------*- C++ -*-===//
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
// A port of LLVM's InstCombiner to SIL. Its main purpose is for performing
// small combining operations/peepholes at the SIL level. It additionally
// performs dead code elimination when it initially adds instructions to the
// work queue in order to reduce compile time by not visiting trivially dead
// instructions.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_PASSMANAGER_SILCOMBINER_H
#define SWIFT_SILOPTIMIZER_PASSMANAGER_SILCOMBINER_H

#include "swift/Basic/Defer.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILInstructionWorklist.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/ClassHierarchyAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/Analysis/ProtocolConformanceAnalysis.h"
#include "swift/SILOptimizer/OptimizerBridging.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/SILOptimizer/Utils/CastOptimizer.h"
#include "swift/SILOptimizer/Utils/Existential.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class AliasAnalysis;

/// This is a class which maintains the state of the combiner and simplifies
/// many operations such as removing/adding instructions and syncing them with
/// the worklist.
class SILCombiner :
    public SILInstructionVisitor<SILCombiner, SILInstruction *> {
  SILFunctionTransform *parentTransform;

  AliasAnalysis *AA;

  BasicCalleeAnalysis *CA;

  DominanceAnalysis *DA;

  /// Determine the set of types a protocol conforms to in whole-module
  /// compilation mode.
  ProtocolConformanceAnalysis *PCA;

  /// Class hierarchy analysis needed to confirm no derived classes of a sole
  /// conforming class.
  ClassHierarchyAnalysis *CHA;

  /// Non local access block analysis that we use when canonicalize object
  /// lifetimes in OSSA.
  NonLocalAccessBlockAnalysis *NLABA;

  /// Worklist containing all of the instructions primed for simplification.
  SmallSILInstructionWorklist<256> Worklist;

  /// Utility for dead code removal.
  InstructionDeleter deleter;

  /// A cache of "dead end blocks" through which all paths it is known that the
  /// program will terminate. This means that we are allowed to leak
  /// objects.
  DeadEndBlocks deadEndBlocks;

  /// Variable to track if the SILCombiner made any changes.
  bool MadeChange;

  /// If set to true then the optimizer is free to erase cond_fail instructions.
  bool RemoveCondFails;

  /// If set to true then copies are canonicalized in OSSA mode.
  bool enableCopyPropagation;

  /// Set to true if some alloc/dealloc_stack instruction are inserted and at
  /// the end of the run stack nesting needs to be corrected.
  bool invalidatedStackNesting = false;

  /// The current iteration of the SILCombine.
  unsigned Iteration;

  // The tracking list is used by `Builder` for newly added
  // instructions, which we will periodically move to our worklist.
  llvm::SmallVector<SILInstruction *, 64> TrackingList;
  
  /// Builder used to insert instructions.
  SILBuilder Builder;

  SILOptFunctionBuilder FuncBuilder;

  /// Cast optimizer
  CastOptimizer CastOpt;

  /// Dead end blocks cache. SILCombine is already not allowed to mess with CFG
  /// edges so it is safe to use this here.
  DeadEndBlocks deBlocks;

  /// External context struct used by \see ownershipRAUWHelper.
  OwnershipFixupContext ownershipFixupContext;
  
  /// For invoking Swift instruction passes.
  SwiftPassInvocation swiftPassInvocation;

public:
  SILCombiner(SILFunctionTransform *parentTransform,
              bool removeCondFails, bool enableCopyPropagation);

  bool runOnFunction(SILFunction &F);

  void clear() {
    Iteration = 0;
    Worklist.resetChecked();
    MadeChange = false;
  }

  /// A "syntactic" high level function that combines our insertPt with the main
  /// builder's builder context.
  ///
  /// Since this is syntactic and we assume that our caller is passing in a
  /// lambda that if we inline will be eliminated, we mark this function always
  /// inline.
  ///
  /// What is nice about this formulation is it enables one to really concisely
  /// create a SILBuilder that uses the SILCombiner's builder context but at a
  /// different use point. Example:
  ///
  /// SILBuilderWithScope builder(insertPt);
  /// builder.createInst1(insertPt->getLoc(), ...);
  /// builder.createInst2(insertPt->getLoc(), ...);
  /// builder.createInst3(insertPt->getLoc(), ...);
  /// auto *finalValue = builder.createInst4(insertPt->getLoc(), ...);
  ///
  /// Thats a lot of typing! Instead, using this API, one can write:
  ///
  /// auto *finalValue = withBuilder(insertPt, [&](auto &b, auto l) {
  ///   b.createInst1(l, ...);
  ///   b.createInst2(l, ...);
  ///   b.createInst3(l, ...);
  ///   return b.createInst4(l, ...);
  /// });
  ///
  /// Since this is meant to be just be syntactic, we always inline this method.
  LLVM_ATTRIBUTE_ALWAYS_INLINE
  SingleValueInstruction *
  withBuilder(SILInstruction *insertPt,
              llvm::function_ref<SingleValueInstruction * (SILBuilder &, SILLocation)> visitor) {
    SILBuilderWithScope builder(insertPt, Builder);
    return visitor(builder, insertPt->getLoc());
  }

  // Insert the instruction New before instruction Old in Old's parent BB. Add
  // New to the worklist.
  SILInstruction *insertNewInstBefore(SILInstruction *New,
                                      SILInstruction &Old) {
    return Worklist.insertNewInstBefore(New, Old);
  }

  // This method is to be used when an instruction is found to be dead,
  // replaceable with another preexisting expression. Here we add all uses of I
  // to the worklist, replace all uses of I with the new value, then return I,
  // so that the combiner will know that I was modified.
  void replaceInstUsesWith(SingleValueInstruction &I, ValueBase *V) {
    return Worklist.replaceInstUsesWith(I, V);
  }

  /// Perform use->set(value) and add use->user to the worklist.
  void setUseValue(Operand *use, SILValue value) {
    use->set(value);
    Worklist.add(use->getUser());
  }

  // This method is to be used when a value is found to be dead,
  // replaceable with another preexisting expression. Here we add all
  // uses of oldValue to the worklist, replace all uses of oldValue
  // with newValue.
  void replaceValueUsesWith(SILValue oldValue, SILValue newValue) {
    Worklist.replaceValueUsesWith(oldValue, newValue);
  }

  void replaceInstUsesPairwiseWith(SILInstruction *oldI, SILInstruction *newI) {
    Worklist.replaceInstUsesPairwiseWith(oldI, newI);
  }

  // Some instructions can never be "trivially dead" due to side effects or
  // producing a void value. In those cases, since we cannot rely on
  // SILCombines trivially dead instruction DCE in order to delete the
  // instruction, visit methods should use this method to delete the given
  // instruction and upon completion of their peephole return the value returned
  // by this method.
  SILInstruction *eraseInstFromFunction(SILInstruction &I,
                                        SILBasicBlock::iterator &InstIter,
                                        bool AddOperandsToWorklist = true) {
    Worklist.eraseInstFromFunction(I, InstIter, AddOperandsToWorklist);
    MadeChange = true;
    // Dummy return, so the caller doesn't need to explicitly return nullptr.
    return nullptr;
  }

  // Erases \p inst and all of its users, recursively.
  // The caller has to make sure that all users are removable (e.g. dead).
  void eraseInstIncludingUsers(SILInstruction *inst);

  SILInstruction *eraseInstFromFunction(SILInstruction &I,
                                        bool AddOperandsToWorklist = true) {
    SILBasicBlock::iterator nullIter;
    return eraseInstFromFunction(I, nullIter, AddOperandsToWorklist);
  }

  void addInitialGroup(ArrayRef<SILInstruction *> List) {
    Worklist.addInitialGroup(List);
  }

  /// Base visitor that does not do anything.
  SILInstruction *visitSILInstruction(SILInstruction *I) { return nullptr; }

  /// Instruction visitors.
  SILInstruction *visitReleaseValueInst(ReleaseValueInst *DI);
  SILInstruction *visitRetainValueInst(RetainValueInst *CI);
  SILInstruction *visitPartialApplyInst(PartialApplyInst *AI);
  SILInstruction *visitApplyInst(ApplyInst *AI);
  SILInstruction *visitBeginApplyInst(BeginApplyInst *BAI);
  SILInstruction *visitTryApplyInst(TryApplyInst *AI);
  SILInstruction *optimizeStringObject(BuiltinInst *BI);
  SILInstruction *visitBuiltinInst(BuiltinInst *BI);
  SILInstruction *visitCondFailInst(CondFailInst *CFI);
  SILInstruction *visitCopyValueInst(CopyValueInst *cvi);
  SILInstruction *visitDestroyValueInst(DestroyValueInst *dvi);
  SILInstruction *visitRefToRawPointerInst(RefToRawPointerInst *RRPI);
  SILInstruction *visitUpcastInst(UpcastInst *UCI);

  // NOTE: The load optimized in this method is a load [trivial].
  SILInstruction *optimizeLoadFromStringLiteral(LoadInst *li);

  SILInstruction *visitLoadBorrowInst(LoadBorrowInst *LI);
  SILInstruction *visitIndexAddrInst(IndexAddrInst *IA);
  bool optimizeStackAllocatedEnum(AllocStackInst *AS);
  SILInstruction *visitAllocStackInst(AllocStackInst *AS);
  SILInstruction *visitAllocRefInst(AllocRefInst *AR);
  SILInstruction *visitSwitchEnumAddrInst(SwitchEnumAddrInst *SEAI);
  SILInstruction *visitInjectEnumAddrInst(InjectEnumAddrInst *IEAI);
  SILInstruction *visitPointerToAddressInst(PointerToAddressInst *PTAI);
  SILInstruction *visitUncheckedAddrCastInst(UncheckedAddrCastInst *UADCI);
  SILInstruction *visitUncheckedRefCastInst(UncheckedRefCastInst *URCI);
  SILInstruction *visitEndCOWMutationInst(EndCOWMutationInst *URCI);
  SILInstruction *visitUncheckedRefCastAddrInst(UncheckedRefCastAddrInst *URCI);
  SILInstruction *visitBridgeObjectToRefInst(BridgeObjectToRefInst *BORI);
  SILInstruction *visitUnconditionalCheckedCastInst(
                    UnconditionalCheckedCastInst *UCCI);
  SILInstruction *
  visitUnconditionalCheckedCastAddrInst(UnconditionalCheckedCastAddrInst *UCCAI);
  SILInstruction *visitRawPointerToRefInst(RawPointerToRefInst *RPTR);
  SILInstruction *
  visitUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *TEDAI);
  SILInstruction *visitCondBranchInst(CondBranchInst *CBI);
  SILInstruction *
  visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *UTBCI);
  SILInstruction *
  visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *UBCI);
  SILInstruction *visitSelectEnumInst(SelectEnumInst *EIT);
  SILInstruction *visitSelectEnumAddrInst(SelectEnumAddrInst *EIT);
  SILInstruction *visitAllocExistentialBoxInst(AllocExistentialBoxInst *S);
  SILInstruction *visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *TTOCMI);
  SILInstruction *visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *OCTTMI);
  SILInstruction *visitTupleExtractInst(TupleExtractInst *TEI);
  SILInstruction *visitFixLifetimeInst(FixLifetimeInst *FLI);
  SILInstruction *visitSwitchValueInst(SwitchValueInst *SVI);
  SILInstruction *
  visitCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *CCABI);
  SILInstruction *
  visitCheckedCastBranchInst(CheckedCastBranchInst *CBI);
  SILInstruction *visitUnreachableInst(UnreachableInst *UI);
  SILInstruction *visitAllocRefDynamicInst(AllocRefDynamicInst *ARDI);
      
  SILInstruction *visitMarkDependenceInst(MarkDependenceInst *MDI);
  SILInstruction *visitClassifyBridgeObjectInst(ClassifyBridgeObjectInst *CBOI);
  SILInstruction *visitConvertFunctionInst(ConvertFunctionInst *CFI);
  SILInstruction *
  visitConvertEscapeToNoEscapeInst(ConvertEscapeToNoEscapeInst *Cvt);
  SILInstruction *
  visitDifferentiableFunctionExtractInst(DifferentiableFunctionExtractInst *DFEI);
  
  SILInstruction *visitPackLengthInst(PackLengthInst *PLI);
  SILInstruction *visitPackElementGetInst(PackElementGetInst *PEGI);
  SILInstruction *visitTuplePackElementAddrInst(TuplePackElementAddrInst *TPEAI);
  SILInstruction *visitCopyAddrInst(CopyAddrInst *CAI);

  SILInstruction *legacyVisitGlobalValueInst(GlobalValueInst *globalValue);

#define PASS(ID, TAG, DESCRIPTION)
#define SWIFT_FUNCTION_PASS(ID, TAG, DESCRIPTION)
#define SWIFT_SILCOMBINE_PASS(INST) \
  SILInstruction *visit##INST(INST *);
#include "swift/SILOptimizer/PassManager/Passes.def"

  /// Instruction visitor helpers.

  // Optimize the "isConcrete" builtin.
  SILInstruction *optimizeBuiltinIsConcrete(BuiltinInst *I);

  SILInstruction *optimizeBuiltinCOWBufferForReading(BuiltinInst *BI);
  SILInstruction *optimizeBuiltinCOWBufferForReadingNonOSSA(BuiltinInst *BI);
  SILInstruction *optimizeBuiltinCOWBufferForReadingOSSA(BuiltinInst *BI);

  // Optimize the "trunc_N1_M2" builtin. if N1 is a result of "zext_M1_*" and
  // the following holds true: N1 > M1 and M2>= M1
  SILInstruction *optimizeBuiltinTruncOrBitCast(BuiltinInst *I);

  // Optimize the "zext_M2_M3" builtin. if M2 is a result of "zext_M1_M2"
  SILInstruction *optimizeBuiltinZextOrBitCast(BuiltinInst *I);

  // Optimize the "cmp_eq_XXX" builtin. If \p NegateResult is true then negate
  // the result bit.
  SILInstruction *optimizeBuiltinCompareEq(BuiltinInst *AI, bool NegateResult);

  SILInstruction *optimizeApplyOfConvertFunctionInst(FullApplySite AI,
                                                     ConvertFunctionInst *CFI);

  bool tryOptimizeKeypath(ApplyInst *AI);
  bool tryOptimizeInoutKeypath(BeginApplyInst *AI);
  bool tryOptimizeKeypathApplication(ApplyInst *AI, SILFunction *callee);
  bool tryOptimizeKeypathOffsetOf(ApplyInst *AI, FuncDecl *calleeFn,
                                  KeyPathInst *kp);
  bool tryOptimizeKeypathKVCString(ApplyInst *AI, FuncDecl *calleeFn,
                                  KeyPathInst *kp);

  /// Sinks owned forwarding instructions to their uses if they do not have
  /// non-debug non-consuming uses. Deletes any debug_values and destroy_values
  /// when this is done. Returns true if we deleted svi and thus we should not
  /// try to visit it.
  bool trySinkOwnedForwardingInst(SingleValueInstruction *svi);

  /// Apply CanonicalizeOSSALifetime to the extended lifetime of any copy
  /// introduced during SILCombine for an owned value.
  void canonicalizeOSSALifetimes(SILInstruction *currentInst);

  // Optimize concatenation of string literals.
  // Constant-fold concatenation of string literals known at compile-time.
  SILInstruction *optimizeConcatenationOfStringLiterals(ApplyInst *AI);

  // Optimize an application of f_inverse(f(x)) -> x.
  bool optimizeIdentityCastComposition(ApplyInst *FInverse,
                                       StringRef FInverseName, StringRef FName);

  /// Let \p user and \p value be two forwarding single value instructions  with
  /// the property that \p value is the value that \p user forwards. In this
  /// case, this helper routine will eliminate \p value if it can rewrite user
  /// in terms of \p newValue. This is intended to handle cases where we have
  /// completely different types so we need to actually create a new instruction
  /// with a different result type.
  ///
  /// \param newValueGenerator Generator that produces the new value to
  /// use. Conditionally called if we can perform the optimization.
  SILInstruction *tryFoldComposedUnaryForwardingInstChain(
      SingleValueInstruction *user, SingleValueInstruction *value,
      function_ref<SILValue()> newValueGenerator);

  SILInstruction *optimizeAlignment(PointerToAddressInst *ptrAdrInst);

  InstModCallbacks &getInstModCallbacks() { return deleter.getCallbacks(); }

private:
  // Build concrete existential information using findInitExistential.
  Optional<ConcreteOpenedExistentialInfo>
  buildConcreteOpenedExistentialInfo(Operand &ArgOperand);

  // Build concrete existential information using SoleConformingType.
  Optional<ConcreteOpenedExistentialInfo>
  buildConcreteOpenedExistentialInfoFromSoleConformingType(Operand &ArgOperand);

  // Common utility function to build concrete existential information for all
  // arguments of an apply instruction.
  void buildConcreteOpenedExistentialInfos(
      FullApplySite Apply,
      llvm::SmallDenseMap<unsigned, ConcreteOpenedExistentialInfo> &COEIs,
      SILBuilderContext &BuilderCtx);

  bool canReplaceArg(FullApplySite Apply, const OpenedArchetypeInfo &OAI,
                     const ConcreteExistentialInfo &CEI, unsigned ArgIdx);
  SILValue canCastArg(FullApplySite Apply, const OpenedArchetypeInfo &OAI,
                      const ConcreteExistentialInfo &CEI, unsigned ArgIdx);

  SILInstruction *createApplyWithConcreteType(
      FullApplySite Apply,
      const llvm::SmallDenseMap<unsigned, ConcreteOpenedExistentialInfo> &COEIs,
      SILBuilderContext &BuilderCtx);

  // Common utility function to replace the WitnessMethodInst using a
  // BuilderCtx.
  void replaceWitnessMethodInst(WitnessMethodInst *WMI,
                                SILBuilderContext &BuilderCtx,
                                CanType ConcreteType,
                                const ProtocolConformanceRef ConformanceRef);

  SILInstruction *
  propagateConcreteTypeOfInitExistential(FullApplySite Apply,
                                         WitnessMethodInst *WMI);

  SILInstruction *propagateConcreteTypeOfInitExistential(FullApplySite Apply);

  /// Propagate concrete types from ProtocolConformanceAnalysis.
  SILInstruction *propagateSoleConformingType(FullApplySite Apply,
                                              WitnessMethodInst *WMI);

  /// Perform one SILCombine iteration.
  bool doOneIteration(SILFunction &F, unsigned Iteration);

  /// Add reachable code to the worklist. Meant to be used when starting to
  /// process a new function.
  void addReachableCodeToWorklist(SILBasicBlock *BB);

  typedef SmallVector<SILInstruction*, 4> UserListTy;

  /// Returns a list of instructions that project or perform reference
  /// counting operations on \p Value or on its uses.
  /// \return return false if \p Value has other than ARC uses.
  static bool recursivelyCollectARCUsers(UserListTy &Uses, ValueBase *Value);

  /// Erases an apply instruction including all it's uses \p.
  /// Inserts release/destroy instructions for all owner and in-parameters.
  /// \return Returns true if successful.
  bool eraseApply(FullApplySite FAS, const UserListTy &Users);

  /// Returns true if the results of a try_apply are not used.
  static bool isTryApplyResultNotUsed(UserListTy &AcceptedUses,
                                      TryApplyInst *TAI);

  bool hasOwnership() const {
    return Builder.hasOwnership();
  }
  
  void runSwiftInstructionPass(SILInstruction *inst,
                               void (*runFunction)(BridgedInstructionPassCtxt));

};

} // end namespace swift

#endif
