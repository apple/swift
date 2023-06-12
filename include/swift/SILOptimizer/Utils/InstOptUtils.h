//===--- InstOptUtils.h - SILOptimizer instruction utilities ----*- C++ -*-===//
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
/// Utilities used by the SILOptimizer for analyzing and transforming
/// SILInstructions.
///
/// SIL/InstUtils.h provides essential SILInstruction utilities.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_INSTOPTUTILS_H
#define SWIFT_SILOPTIMIZER_UTILS_INSTOPTUTILS_H

#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/ClassHierarchyAnalysis.h"
#include "swift/SILOptimizer/Analysis/EpilogueARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/SimplifyInstruction.h"
#include "swift/SILOptimizer/Utils/InstModCallbacks.h"
#include "llvm/ADT/SmallPtrSet.h"

namespace swift {

class DominanceInfo;
class DeadEndBlocks;
class BasicCalleeAnalysis;
template <class T> class NullablePtr;

/// Transform a Use Range (Operand*) into a User Range (SILInstruction *)
using UserTransform = std::function<SILInstruction *(Operand *)>;
using ValueBaseUserRange =
    TransformRange<iterator_range<ValueBase::use_iterator>, UserTransform>;

template <typename Range>
inline TransformRange<Range, UserTransform> makeUserRange(Range range) {
  auto toUser = [](Operand *operand) { return operand->getUser(); };
  return makeTransformRange(range, UserTransform(toUser));
}

/// Transform a use_iterator range (Operand*) into an llvm::iterator_range
/// of users (SILInstruction *)
inline iterator_range<llvm::mapped_iterator<ValueBase::use_iterator, UserTransform>>
makeUserIteratorRange(iterator_range<ValueBase::use_iterator> useRange) {
  auto toUser = [](Operand *operand) { return operand->getUser(); };
  return llvm::map_range(useRange, UserTransform(toUser));
}

using DeadInstructionSet = llvm::SmallSetVector<SILInstruction *, 8>;

/// Create a retain of \p Ptr before the \p InsertPt.
NullablePtr<SILInstruction> createIncrementBefore(SILValue ptr,
                                                  SILInstruction *insertpt);

/// Create a release of \p Ptr before the \p InsertPt.
NullablePtr<SILInstruction> createDecrementBefore(SILValue ptr,
                                                  SILInstruction *insertpt);

/// Get the insertion point after \p val.
Optional<SILBasicBlock::iterator> getInsertAfterPoint(SILValue val);

/// True if this instruction's only uses are debug_value (in -O mode),
/// destroy_value, end_lifetime or end-of-scope instruction such as end_borrow.
bool hasOnlyEndOfScopeOrEndOfLifetimeUses(SILInstruction *inst);

/// Return the number of @inout arguments passed to the given apply site.
unsigned getNumInOutArguments(FullApplySite applySite);

/// If \c inst is dead, delete it and recursively eliminate all code that
/// becomes dead because of that. If more than one instruction must
/// be checked/deleted use the \c InstructionDeleter utility.
///
/// This function will add necessary compensation code to fix the lifetimes of
/// the operands of the deleted instructions.
///
/// \pre the SIL function containing the instruction is assumed to be
/// consistent, i.e., does not have under or over releases.
///
/// \p callbacks.onDelete() is invoked to delete each instruction.
void eliminateDeadInstruction(SILInstruction *inst,
                              InstModCallbacks callbacks = InstModCallbacks());

/// For each of the given instructions, if they are dead delete them
/// along with their dead operands. Note this utility must be phased out and
/// replaced by \c eliminateDeadInstruction  and
/// \c InstructionDeleter utilities.
///
/// \param inst The ArrayRef of instructions to be deleted.
/// \param force If Force is set, don't check if the top level instructions
///        are considered dead - delete them regardless.
/// \param callbacks The inst mod callbacks used to delete instructions.
///
/// Deprecated: Use InstructionDeleter instead.
void recursivelyDeleteTriviallyDeadInstructions(
    ArrayRef<SILInstruction *> inst, bool force = false,
    InstModCallbacks callbacks = InstModCallbacks());

/// If the given instruction is dead, delete it along with its dead
/// operands. Note this utility must be phased out and replaced by
/// \c eliminateDeadInstruction and
/// \c InstructionDeleter utilities.
///
/// \param inst The instruction to be deleted.
/// \param force If Force is set, don't check if the top level instruction is
///        considered dead - delete it regardless.
/// \param callbacks InstModCallback used to delete instructions.
///
/// Deprecated: Use InstructionDeleter instead.
void recursivelyDeleteTriviallyDeadInstructions(
    SILInstruction *inst, bool force = false,
    InstModCallbacks callbacks = InstModCallbacks());

/// True if this instruction can be deleted if all its uses can also be deleted.
bool isInstructionTriviallyDeletable(SILInstruction *inst);

/// Perform a fast local check to see if the instruction is dead.
///
/// This routine only examines the state of the instruction at hand.
bool isInstructionTriviallyDead(SILInstruction *inst);

/// Return true if this is a release instruction that's not going to
/// free the object.
bool isIntermediateRelease(SILInstruction *inst, EpilogueARCFunctionInfo *erfi);

/// Recursively collect all the uses and transitive uses of the
/// instruction.
void collectUsesOfValue(SILValue V,
                        llvm::SmallPtrSetImpl<SILInstruction *> &Insts);

/// Recursively erase all of the uses of the value (but not the
/// value itself)
void eraseUsesOfValue(SILValue value);

/// Return true if \p type is a value type (struct/enum) that requires
/// deinitialization beyond destruction of its members.
bool hasValueDeinit(SILType type);

/// Return true if \p value has a value type (struct/enum) that requires
/// deinitialization beyond destruction of its members.
inline bool hasValueDeinit(SILValue value) {
  return hasValueDeinit(value->getType());
}

/// Gets the concrete value which is stored in an existential box.
/// Returns %value in following pattern:
///
///    %existentialBox = alloc_existential_box $Error, $ConcreteError
///    %a = project_existential_box $ConcreteError in %existentialBox : $Error
///    store %value to %a : $*ConcreteError
///
/// Returns an invalid SILValue in case there are multiple stores or any unknown
/// users of \p existentialBox.
/// The \p ignoreUser is ignored in the user list of \p existentialBox.
SILValue
getConcreteValueOfExistentialBox(AllocExistentialBoxInst *existentialBox,
                                 SILInstruction *ignoreUser);

/// Gets the concrete value which is stored in an existential box, which itself
/// is stored in \p addr.
/// Returns %value in following pattern:
///
///    %b = alloc_existential_box $Error, $ConcreteError
///    %a = project_existential_box $ConcreteError in %b : $Error
///    store %value to %a : $*ConcreteError
///    %addr = alloc_stack $Error
///    store %b to %addr : $*Error
///
/// Returns an invalid SILValue in case there are multiple stores or any unknown
/// users of \p addr or the existential box.
/// The \p ignoreUser is ignored in the user list of \p addr.
SILValue getConcreteValueOfExistentialBoxAddr(SILValue addr,
                                              SILInstruction *ignoreUser);

/// Cast a value into the expected, ABI compatible type if necessary.
/// This may happen e.g. when:
/// - a type of the return value is a subclass of the expected return type.
/// - actual return type and expected return type differ in optionality.
/// - both types are tuple-types and some of the elements need to be casted.
///
/// \p usePoints is required when \p value has guaranteed ownership. It must be
/// the last users of the returned, casted value. A usePoint cannot be a
/// BranchInst (a phi is never the last guaranteed user). \p builder's current
/// insertion point must dominate all \p usePoints.
std::pair<SILValue, bool /* changedCFG */>
castValueToABICompatibleType(SILBuilder *builder, SILLocation Loc,
                             SILValue value, SILType srcTy, SILType destTy,
                             ArrayRef<SILInstruction *> usePoints);
/// Peek through trivial Enum initialization, typically for pointless
/// Optionals.
///
/// The returned InitEnumDataAddr dominates the given
/// UncheckedTakeEnumDataAddrInst.
InitEnumDataAddrInst *
findInitAddressForTrivialEnum(UncheckedTakeEnumDataAddrInst *utedai);

/// Returns a project_box if it is the next instruction after \p ABI and
/// and has \p ABI as operand. Otherwise it creates a new project_box right
/// after \p ABI and returns it.
ProjectBoxInst *getOrCreateProjectBox(AllocBoxInst *abi, unsigned index);

/// Return true if any call inside the given function may bind dynamic
/// 'Self' to a generic argument of the callee.
bool mayBindDynamicSelf(SILFunction *f);

/// Check whether the \p addr is an address of a tail-allocated array element.
bool isAddressOfArrayElement(SILValue addr);

/// Move an ApplyInst's FuncRef so that it dominates the call site.
void placeFuncRef(ApplyInst *ai, DominanceInfo *dt);

/// Add arguments, \p vals, to the branch-edge that is pointing into
/// block \p Dest. Return a new instruction and do not erase the old
/// instruction.
TermInst *addArgumentsToBranch(ArrayRef<SILValue> vals, SILBasicBlock *dest,
                               TermInst *branch);

/// Get the linkage to be used for specializations of a function with
/// the given linkage.
SILLinkage getSpecializedLinkage(SILFunction *f, SILLinkage linkage);

/// Tries to perform jump-threading on all checked_cast_br instruction in
/// function \p Fn.
bool tryCheckedCastBrJumpThreading(
    SILFunction *fn, DominanceInfo *dt, DeadEndBlocks *deBlocks,
    SmallVectorImpl<SILBasicBlock *> &blocksForWorklist,
    bool EnableOSSARewriteTerminator);

/// Get all consumed arguments of a partial_apply.
///
/// These are basically all arguments, except inout arguments and arguments
/// of trivial type.
/// If \p includeTrivialAddrArgs is true, also trivial address-type arguments
/// are included.
void getConsumedPartialApplyArgs(PartialApplyInst *pai,
                                 SmallVectorImpl<Operand *> &argOperands,
                                 bool includeTrivialAddrArgs);

/// Emit destroy operation for \p operand, and call appropriate functions from
/// \p callbacks for newly created instructions and deleted instructions.
void emitDestroyOperation(SILBuilder &builder, SILLocation loc,
                          SILValue operand, InstModCallbacks callbacks);

/// Collect all (transitive) users of \p inst which just copy or destroy \p
/// inst.
///
/// In other words: all users which do not prevent \p inst from being considered
/// as "dead".
/// Returns true, if there are no other users beside those collected in \p
/// destroys, i.e. if \p inst can be considered as "dead".
bool collectDestroys(SingleValueInstruction *inst,
                     SmallVectorImpl<Operand *> &destroys);

/// If Closure is a partial_apply or thin_to_thick_function with only local
/// ref count users and a set of post-dominating releases:
///
/// 1. Remove all ref count operations and the closure.
/// 2. At each one of the last release locations insert releases for the
///    captured args if we have a partial_apply (except \p needKeepArgsAlive is
///    false).
///
/// In the future this should be extended to be less conservative with users.
bool tryDeleteDeadClosure(SingleValueInstruction *closure,
                          InstModCallbacks callbacks = InstModCallbacks(),
                          bool needKeepArgsAlive = true);

/// Given a SILValue argument to a partial apply \p Arg and the associated
/// parameter info for that argument, perform the necessary cleanups to Arg when
/// one is attempting to delete the partial apply.
void releasePartialApplyCapturedArg(
    SILBuilder &builder, SILLocation loc, SILValue arg,
    SILParameterInfo paramInfo,
    InstModCallbacks callbacks = InstModCallbacks());

/// Insert destroys of captured arguments of partial_apply [stack].
void insertDestroyOfCapturedArguments(
    PartialApplyInst *pai, SILBuilder &builder,
    llvm::function_ref<bool(SILValue)> shouldInsertDestroy =
        [](SILValue arg) -> bool { return true; },
    SILLocation loc = RegularLocation::getAutoGeneratedLocation());

void insertDeallocOfCapturedArguments(PartialApplyInst *pai,
                                      DominanceInfo *domInfo);

/// This iterator 'looks through' one level of builtin expect users exposing all
/// users of the looked through builtin expect instruction i.e it presents a
/// view that shows all users as if there were no builtin expect instructions
/// interposed.
class IgnoreExpectUseIterator {
public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = Operand*;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type*;
  using reference = value_type&;    

private:
  ValueBaseUseIterator origUseChain;
  ValueBaseUseIterator currentIter;

  static BuiltinInst *isExpect(Operand *use) {
    if (auto *bi = dyn_cast<BuiltinInst>(use->getUser()))
      if (bi->getIntrinsicInfo().ID == llvm::Intrinsic::expect)
        return bi;
    return nullptr;
  }

  // Advance through expect users to their users until we encounter a user that
  // is not an expect.
  void advanceThroughExpects() {
    while (currentIter == origUseChain
           && currentIter != ValueBaseUseIterator(nullptr)) {
      auto *Expect = isExpect(*currentIter);
      if (!Expect)
        return;
      currentIter = Expect->use_begin();
      // Expect with no users advance to next item in original use chain.
      if (currentIter == Expect->use_end())
        currentIter = ++origUseChain;
    }
  }

public:
  IgnoreExpectUseIterator(ValueBase *value)
      : origUseChain(value->use_begin()), currentIter(value->use_begin()) {
    advanceThroughExpects();
  }

  IgnoreExpectUseIterator() = default;

  Operand *operator*() const { return *currentIter; }
  Operand *operator->() const { return *currentIter; }
  SILInstruction *getUser() const { return currentIter->getUser(); }

  IgnoreExpectUseIterator &operator++() {
    assert(**this && "increment past end()!");
    if (origUseChain == currentIter) {
      // Use chain of the original value.
      ++origUseChain;
      ++currentIter;
      // Ignore expects.
      advanceThroughExpects();
    } else {
      // Use chain of an expect.
      ++currentIter;
      if (currentIter == ValueBaseUseIterator(nullptr)) {
        // At the end of the use chain of an expect.
        currentIter = ++origUseChain;
        advanceThroughExpects();
      }
    }
    return *this;
  }

  IgnoreExpectUseIterator operator++(int unused) {
    IgnoreExpectUseIterator copy = *this;
    ++*this;
    return copy;
  }
  friend bool operator==(IgnoreExpectUseIterator lhs,
                         IgnoreExpectUseIterator rhs) {
    return lhs.currentIter == rhs.currentIter;
  }
  friend bool operator!=(IgnoreExpectUseIterator lhs,
                         IgnoreExpectUseIterator rhs) {
    return !(lhs == rhs);
  }
};

inline iterator_range<IgnoreExpectUseIterator>
ignore_expect_uses(ValueBase *value) {
  return make_range(IgnoreExpectUseIterator(value), IgnoreExpectUseIterator());
}

/// Run simplifyInstruction() on all of the instruction I's users if they only
/// have one result (since simplifyInstruction assumes that). Replace all uses
/// of the user with its simplification of we succeed. Returns true if we
/// succeed and false otherwise.
///
/// An example of how this is useful is in cases where one is splitting up an
/// aggregate and reforming it, the reformed aggregate may have extract
/// operations from it. These can be simplified and removed.
bool simplifyUsers(SingleValueInstruction *inst);

/// True if a type can be expanded without a significant increase to code size.
///
/// False if expanding a type is invalid. For example, expanding a
/// struct-with-deinit drops the deinit.
bool shouldExpand(SILModule &module, SILType ty);

/// Check if the value of value is computed by means of a simple initialization.
/// Store the actual SILValue into \p Val and the reversed list of instructions
/// initializing it in \p Insns.
/// The check is performed by recursively walking the computation of the
/// SIL value being analyzed.
bool analyzeStaticInitializer(SILValue value,
                              SmallVectorImpl<SILInstruction *> &insns);

/// Returns true if the below operation will succeed.
bool canReplaceLoadSequence(SILInstruction *inst);

/// Replace load sequence which may contain
/// a chain of struct_element_addr followed by a load.
/// The sequence is traversed inside out, i.e.
/// starting with the innermost struct_element_addr
void replaceLoadSequence(SILInstruction *inst, SILValue value);

/// Do we have enough information to determine all callees that could
/// be reached by calling the function represented by Decl?
bool calleesAreStaticallyKnowable(SILModule &module, SILDeclRef decl);

/// Do we have enough information to determine all callees that could
/// be reached by calling the function represented by Decl?
bool calleesAreStaticallyKnowable(SILModule &module, ValueDecl *vd);

// Attempt to get the instance for , whose static type is the same as
// its exact dynamic type, returning a null SILValue() if we cannot find it.
// The information that a static type is the same as the exact dynamic,
// can be derived e.g.:
// - from a constructor or
// - from a successful outcome of a checked_cast_br [exact] instruction.
SILValue getInstanceWithExactDynamicType(SILValue instance,
                                         ClassHierarchyAnalysis *cha);

/// Try to determine the exact dynamic type of an object.
/// returns the exact dynamic type of the object, or an empty type if the exact
/// type could not be determined.
SILType getExactDynamicType(SILValue instance, ClassHierarchyAnalysis *cha,
                            bool forUnderlyingObject = false);

/// Try to statically determine the exact dynamic type of the underlying object.
/// returns the exact dynamic type of the underlying object, or an empty SILType
/// if the exact type could not be determined.
SILType getExactDynamicTypeOfUnderlyingObject(SILValue instance,
                                              ClassHierarchyAnalysis *cha);

// Move only data structure that is the result of findLocalApplySite.
///
/// NOTE: Generally it is not suggested to have move only types that contain
/// small vectors. Since our small vectors contain one element or a std::vector
/// like data structure , this is ok since we will either just copy the single
/// element when we do the move or perform a move of the vector type.
struct LLVM_LIBRARY_VISIBILITY FindLocalApplySitesResult {
  /// Contains the list of local non fully applied partial apply sites that we
  /// found.
  SmallVector<ApplySite, 1> partialApplySites;

  /// Contains the list of full apply sites that we found.
  SmallVector<FullApplySite, 1> fullApplySites;

  /// Set to true if the function_ref escapes into a use that our analysis does
  /// not understand. Set to false if we found a use that had an actual
  /// escape. Set to None if we did not find any call sites, but also didn't
  /// find any "escaping uses" as well.
  ///
  /// The none case is so that we can distinguish in between saying that a value
  /// did escape and saying that we did not find any conservative information.
  bool escapes;

  FindLocalApplySitesResult() = default;
  FindLocalApplySitesResult(const FindLocalApplySitesResult &) = delete;
  FindLocalApplySitesResult &
  operator=(const FindLocalApplySitesResult &) = delete;
  FindLocalApplySitesResult(FindLocalApplySitesResult &&) = default;
  FindLocalApplySitesResult &operator=(FindLocalApplySitesResult &&) = default;
  ~FindLocalApplySitesResult() = default;

  /// Treat this function ref as escaping only if we found an actual user we
  /// didn't understand. Do not treat it as escaping if we did not find any
  /// users at all.
  bool isEscaping() const { return escapes; }
};

/// Returns .some(FindLocalApplySitesResult) if we found any interesting
/// information for the given function_ref. Otherwise, returns None.
///
/// We consider "interesting information" to mean inclusively that:
///
/// 1. We discovered that the function_ref never escapes.
/// 2. We were able to find either a partial apply or a full apply site.
Optional<FindLocalApplySitesResult>
findLocalApplySites(FunctionRefBaseInst *fri);

/// Gets the base implementation of a method.
AbstractFunctionDecl *getBaseMethod(AbstractFunctionDecl *FD);

bool tryOptimizeApplyOfPartialApply(
    PartialApplyInst *pai, SILBuilderContext &builderCtxt,
    InstModCallbacks callbacks = InstModCallbacks());

/// Clone this full apply site, replacing the callee with \p newCallee while
/// doing so.
///
/// The current full apply site is used as an insertion point, so the caller
/// must clean up this full apply site.
FullApplySite cloneFullApplySiteReplacingCallee(FullApplySite applySite,
                                                SILValue newCallee,
                                                SILBuilderContext &builderCtx);

/// Replace all uses of \p oldValue with \p newValue, notifying the callbacks
/// of new uses and when end-of-scope instructions are deleted.
SILBasicBlock::iterator replaceAllUses(SILValue oldValue, SILValue newValue,
                                       SILBasicBlock::iterator nextii,
                                       InstModCallbacks &callbacks);

/// This is a low level routine that makes all uses of \p svi uses of \p
/// newValue (ignoring end scope markers) and then deletes \p svi and all end
/// scope markers. Then returns the next inst to process.
SILBasicBlock::iterator replaceAllUsesAndErase(SingleValueInstruction *svi,
                                               SILValue newValue,
                                               InstModCallbacks &callbacks);

/// Replace all uses of \p oldValue with \p newValue, delete the instruction
/// that defines \p oldValue, and notify the callbacks of new uses and when
/// the defining instruction and its end-of-scope instructions are deleted.
///
/// Precondition: \p oldValue must be a SingleValueInstruction or a terminator
/// result. \p oldValue must be the only result with remaining uses. For
/// terminators with multiple results, remove all other results for, e.g. via
/// replaceAllUsesWithUndef().
///
/// If \p oldValue is a terminator result, a new branch instruction is inserted
/// in place of the old terminator and all basic block successors become
/// unreachable except for the successor containing the replaced result.
SILBasicBlock::iterator replaceAllUsesAndErase(SILValue oldValue,
                                               SILValue newValue,
                                               InstModCallbacks &callbacks);

/// This API is equivalent to performing \p use->set(\p newValue) except that:
///
/// 1. If the user of \p use is an end scope, this API no-opts. This API is only
///    used in contexts where we are rewriting uses and are not interesting in
///    end scope instructions since we are moving uses from one scope to another
///    scope.
///
/// 2. If the user of \p use is not an end scope, but is a lifetime ending use
///    of \p use->get(), we insert a destroy_value|end_borrow as appropriate on
///    \p use->get() to ensure \p use->get()'s lifetime is still ended. We
///    assume that if \p use->getUser() is lifetime ending, that our caller has
///    ensured that we can end \p newValue's lifetime.
SILBasicBlock::iterator replaceSingleUse(Operand *use, SILValue newValue,
                                         InstModCallbacks &callbacks);

/// Creates a copy of \p value and inserts additional control equivalent copy
/// and destroy at leaking blocks to adjust ownership and make available for use
/// at \p inBlock.
SILValue
makeCopiedValueAvailable(SILValue value, SILBasicBlock *inBlock);

/// Given an existing @owned value \p value, this utility
/// inserts control equivalent copy and destroy at leaking blocks to adjust
/// ownership and make \p value available for use at \p inBlock.
SILValue makeValueAvailable(SILValue value, SILBasicBlock *inBlock);

/// Given an ssa value \p value, create destroy_values at leaking blocks
///
/// Warning: This does not properly cleanup an OSSA lifetime with a consuming
/// use blocks inside a loop relative to \p value. The client must create
/// separate copies for any uses within the loop.
void endLifetimeAtLeakingBlocks(SILValue value,
                                ArrayRef<SILBasicBlock *> userBBs);

/// Given a forwarding instruction, eliminate it if all of its users are debug
/// instructions and ownership uses.
bool tryEliminateOnlyOwnershipUsedForwardingInst(
    SingleValueInstruction *forwardingInst, InstModCallbacks &callbacks);

/// Constant-fold the Builtin.canBeClass if the type is known.
IntegerLiteralInst *optimizeBuiltinCanBeObjCClass(BuiltinInst *bi,
                                                  SILBuilder &builder);

/// Performs "predictable" memory access optimizations.
///
/// See the PredictableMemoryAccessOptimizations pass.
bool optimizeMemoryAccesses(SILFunction *fn);

/// Performs "predictable" dead allocation optimizations.
///
/// See the PredictableDeadAllocationElimination pass.
bool eliminateDeadAllocations(SILFunction *fn);

bool specializeAppliesInFunction(SILFunction &F,
                                 SILTransform *transform,
                                 bool isMandatory);
} // end namespace swift

#endif // SWIFT_SILOPTIMIZER_UTILS_INSTOPTUTILS_H
