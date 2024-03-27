//===--- OptimizerBridging.h - header for the OptimizerBridging module ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_OPTIMIZERBRIDGING_H
#define SWIFT_SILOPTIMIZER_OPTIMIZERBRIDGING_H

// Do not add other C++/llvm/swift header files here!
// Function implementations should be placed into OptimizerBridgingImpl.h or PassManager.cpp
// (under OptimizerBridging) andrequired header files should be added there.
//
// Pure bridging mode does not permit including any C++/llvm/swift headers.
// See also the comments for `BRIDGING_MODE` in the top-level CMakeLists.txt file.
//
#include "swift/AST/ASTBridging.h"
#include "swift/SIL/SILBridging.h"

#ifdef USED_IN_CPP_SOURCE

#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"

#else // USED_IN_CPP_SOURCE

// Pure bridging mode does not permit including any C++/llvm/swift headers.
// See also the comments for `BRIDGING_MODE` in the top-level CMakeLists.txt file.
#ifdef SWIFT_SIL_SILVALUE_H
#error "should not include swift headers into bridging header"
#endif
#ifdef LLVM_SUPPORT_COMPILER_H
#error "should not include llvm headers into bridging header"
#endif

#endif // USED_IN_CPP_SOURCE

SWIFT_BEGIN_NULLABILITY_ANNOTATIONS

namespace swift {
class AliasAnalysis;
class BasicCalleeAnalysis;
class DeadEndBlocks;
class DominanceInfo;
class PostDominanceInfo;
class BasicBlockSet;
class NodeSet;
class OperandSet;
class ClonerWithFixedLocation;
class SwiftPassInvocation;
class FixedSizeSlabPayload;
class FixedSizeSlab;
class SILVTable;
}

struct BridgedPassContext;

struct BridgedAliasAnalysis {
  swift::AliasAnalysis * _Nonnull aa;

  BRIDGED_INLINE BridgedMemoryBehavior getMemBehavior(BridgedInstruction inst, BridgedValue addr) const;

  typedef BridgedMemoryBehavior (* _Nonnull GetMemEffectFn)(
        BridgedPassContext context, BridgedValue, BridgedInstruction, SwiftInt);
  typedef bool (* _Nonnull Escaping2InstFn)(
        BridgedPassContext context, BridgedValue, BridgedInstruction, SwiftInt);
  typedef bool (* _Nonnull Escaping2ValFn)(
        BridgedPassContext context, BridgedValue, BridgedValue);
  typedef bool (* _Nonnull Escaping2ValIntFn)(
        BridgedPassContext context, BridgedValue, BridgedValue, SwiftInt);

  static void registerAnalysis(GetMemEffectFn getMemEffectsFn,
                               Escaping2InstFn isObjReleasedFn,
                               Escaping2ValIntFn isAddrVisibleFromObjFn,
                               Escaping2ValFn mayPointToSameAddrFn);
};

struct BridgedCalleeAnalysis {
  swift::BasicCalleeAnalysis * _Nonnull ca;

  struct CalleeList {
    uint64_t storage[3];

#ifdef USED_IN_CPP_SOURCE
    CalleeList(swift::CalleeList list) {
      *reinterpret_cast<swift::CalleeList *>(&storage) = list;
    }
    swift::CalleeList unbridged() const {
      return *reinterpret_cast<const swift::CalleeList *>(&storage);
    }
#endif

    BRIDGED_INLINE bool isIncomplete() const;
    BRIDGED_INLINE SwiftInt getCount() const;
    SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedFunction getCallee(SwiftInt index) const;
  };

  SWIFT_IMPORT_UNSAFE CalleeList getCallees(BridgedValue callee) const;
  SWIFT_IMPORT_UNSAFE CalleeList getDestructors(BridgedType type, bool isExactType) const;

  typedef bool (* _Nonnull IsDeinitBarrierFn)(BridgedInstruction, BridgedCalleeAnalysis bca);
  typedef BridgedMemoryBehavior (* _Nonnull GetMemBehvaiorFn)(
        BridgedInstruction apply, bool observeRetains, BridgedCalleeAnalysis bca);

  static void registerAnalysis(IsDeinitBarrierFn isDeinitBarrierFn,
                               GetMemBehvaiorFn getEffectsFn);
};

struct BridgedDeadEndBlocksAnalysis {
  swift::DeadEndBlocks * _Nonnull deb;

  BRIDGED_INLINE bool isDeadEnd(BridgedBasicBlock block) const;
};

struct BridgedDomTree {
  swift::DominanceInfo * _Nonnull di;

  BRIDGED_INLINE bool dominates(BridgedBasicBlock dominating, BridgedBasicBlock dominated) const;
};

struct BridgedPostDomTree {
  swift::PostDominanceInfo * _Nonnull pdi;

  BRIDGED_INLINE bool postDominates(BridgedBasicBlock dominating, BridgedBasicBlock dominated) const;
};

struct BridgedUtilities {
  typedef void (* _Nonnull VerifyFunctionFn)(BridgedPassContext, BridgedFunction);
  typedef void (* _Nonnull UpdateBorrowedFromFn)(BridgedPassContext, BridgedFunction);
  typedef void (* _Nonnull UpdateBorrowedFromPhisFn)(BridgedPassContext, BridgedArrayRef);

  static void registerVerifier(VerifyFunctionFn verifyFunctionFn);
  static void registerBorrowedFromUpdater(UpdateBorrowedFromFn updateBorrowedFromFn,
                                          UpdateBorrowedFromPhisFn updateBorrowedFromPhisFn);
};

struct BridgedBasicBlockSet {
  swift::BasicBlockSet * _Nonnull set;

  BRIDGED_INLINE bool contains(BridgedBasicBlock block) const;
  BRIDGED_INLINE bool insert(BridgedBasicBlock block) const;
  BRIDGED_INLINE void erase(BridgedBasicBlock block) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedFunction getFunction() const;
};

struct BridgedNodeSet {
  swift::NodeSet * _Nonnull set;

  BRIDGED_INLINE bool containsValue(BridgedValue value) const;
  BRIDGED_INLINE bool insertValue(BridgedValue value) const;
  BRIDGED_INLINE void eraseValue(BridgedValue value) const;
  BRIDGED_INLINE bool containsInstruction(BridgedInstruction inst) const;
  BRIDGED_INLINE bool insertInstruction(BridgedInstruction inst) const;
  BRIDGED_INLINE void eraseInstruction(BridgedInstruction inst) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedFunction getFunction() const;
};

struct BridgedOperandSet {
  swift::OperandSet * _Nonnull set;

  BRIDGED_INLINE bool contains(BridgedOperand operand) const;
  BRIDGED_INLINE bool insert(BridgedOperand operand) const;
  BRIDGED_INLINE void erase(BridgedOperand operand) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedFunction getFunction() const;
};

struct BridgedCloner {
  swift::ClonerWithFixedLocation * _Nonnull cloner;

  BridgedCloner(BridgedGlobalVar var, BridgedPassContext context);
  BridgedCloner(BridgedInstruction inst, BridgedPassContext context);
  void destroy(BridgedPassContext context);
  SWIFT_IMPORT_UNSAFE BridgedValue getClonedValue(BridgedValue v);
  bool isValueCloned(BridgedValue v) const;
  void clone(BridgedInstruction inst);
};

struct BridgedPassContext {
  swift::SwiftPassInvocation * _Nonnull invocation;

  enum class SILStage {
    Raw,
    Canonical,
    Lowered
  };

  SWIFT_IMPORT_UNSAFE BridgedOwnedString getModuleDescription() const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedChangeNotificationHandler asNotificationHandler() const;
  BRIDGED_INLINE void notifyDependencyOnBodyOf(BridgedFunction otherFunction) const;
  BRIDGED_INLINE SILStage getSILStage() const;
  BRIDGED_INLINE bool hadError() const;
  BRIDGED_INLINE bool moduleIsSerialized() const;
  BRIDGED_INLINE bool isTransforming(BridgedFunction function) const;

  // Analysis

  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedAliasAnalysis getAliasAnalysis() const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedCalleeAnalysis getCalleeAnalysis() const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedDeadEndBlocksAnalysis getDeadEndBlocksAnalysis() const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedDomTree getDomTree() const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedPostDomTree getPostDomTree() const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedNominalTypeDecl getSwiftArrayDecl() const;

  // AST

  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE
  BridgedDiagnosticEngine getDiagnosticEngine() const;

  // SIL modifications

  struct DevirtResult {
    OptionalBridgedInstruction newApply;
    bool cfgChanged;
  };

  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedBasicBlock splitBlockBefore(BridgedInstruction bridgedInst) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedBasicBlock splitBlockAfter(BridgedInstruction bridgedInst) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedBasicBlock createBlockAfter(BridgedBasicBlock bridgedBlock) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedBasicBlock appendBlock(BridgedFunction bridgedFunction) const;
  BRIDGED_INLINE void eraseInstruction(BridgedInstruction inst) const;
  BRIDGED_INLINE void eraseBlock(BridgedBasicBlock block) const;
  static BRIDGED_INLINE void moveInstructionBefore(BridgedInstruction inst, BridgedInstruction beforeInst);
  bool tryOptimizeApplyOfPartialApply(BridgedInstruction closure) const;
  bool tryDeleteDeadClosure(BridgedInstruction closure, bool needKeepArgsAlive) const;
  SWIFT_IMPORT_UNSAFE DevirtResult tryDevirtualizeApply(BridgedInstruction apply, bool isMandatory) const;
  bool tryOptimizeKeypath(BridgedInstruction apply) const;
  SWIFT_IMPORT_UNSAFE OptionalBridgedValue constantFoldBuiltin(BridgedInstruction builtin) const;
  SWIFT_IMPORT_UNSAFE swift::SILVTable * _Nullable specializeVTableForType(BridgedType type,
                                                                           BridgedFunction function) const;
  bool specializeClassMethodInst(BridgedInstruction cm) const;
  bool specializeAppliesInFunction(BridgedFunction function, bool isMandatory) const;
  SWIFT_IMPORT_UNSAFE BridgedOwnedString mangleOutlinedVariable(BridgedFunction function) const;
  SWIFT_IMPORT_UNSAFE BridgedOwnedString mangleAsyncRemoved(BridgedFunction function) const;
  SWIFT_IMPORT_UNSAFE BridgedOwnedString mangleWithDeadArgs(const SwiftInt * _Nullable deadArgs,
                                                            SwiftInt numDeadArgs,
                                                            BridgedFunction function) const;

  SWIFT_IMPORT_UNSAFE BridgedGlobalVar createGlobalVariable(BridgedStringRef name, BridgedType type,
                                                            bool isPrivate) const;
  void inlineFunction(BridgedInstruction apply, bool mandatoryInline) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedValue getSILUndef(BridgedType type) const;
  BRIDGED_INLINE bool optimizeMemoryAccesses(BridgedFunction f) const;
  BRIDGED_INLINE bool eliminateDeadAllocations(BridgedFunction f) const;

  // IRGen

  SwiftInt getStaticSize(BridgedType type) const;
  SwiftInt getStaticAlignment(BridgedType type) const;
  SwiftInt getStaticStride(BridgedType type) const;

  // Sets

  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedBasicBlockSet allocBasicBlockSet() const;
  BRIDGED_INLINE void freeBasicBlockSet(BridgedBasicBlockSet set) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedNodeSet allocNodeSet() const;
  BRIDGED_INLINE void freeNodeSet(BridgedNodeSet set) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedOperandSet allocOperandSet() const;
  BRIDGED_INLINE void freeOperandSet(BridgedOperandSet set) const;

  // Stack nesting

  BRIDGED_INLINE void notifyInvalidatedStackNesting() const;
  BRIDGED_INLINE bool getNeedFixStackNesting() const;
  void fixStackNesting(BridgedFunction function) const;

  // Slabs

  struct Slab {
    swift::FixedSizeSlabPayload * _Nullable data = nullptr;

    BRIDGED_INLINE static SwiftInt getCapacity();
    BRIDGED_INLINE Slab(swift::FixedSizeSlab * _Nullable slab);
    BRIDGED_INLINE swift::FixedSizeSlab * _Nullable getSlab() const;
    SWIFT_IMPORT_UNSAFE BRIDGED_INLINE Slab getNext() const;
    SWIFT_IMPORT_UNSAFE BRIDGED_INLINE Slab getPrevious() const;
  };

  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE Slab allocSlab(Slab afterSlab) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE Slab freeSlab(Slab slab) const;

  // Access SIL module data structures

  struct VTableArray {
    swift::SILVTable * const _Nonnull * _Nullable base;
    SwiftInt count;
  };

  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE OptionalBridgedFunction getFirstFunctionInModule() const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE static OptionalBridgedFunction getNextFunctionInModule(BridgedFunction function);
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE OptionalBridgedGlobalVar getFirstGlobalInModule() const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE static OptionalBridgedGlobalVar getNextGlobalInModule(BridgedGlobalVar global);
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE VTableArray getVTables() const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE OptionalBridgedWitnessTable getFirstWitnessTableInModule() const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE static OptionalBridgedWitnessTable getNextWitnessTableInModule(
                                                                                  BridgedWitnessTable table);
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE OptionalBridgedDefaultWitnessTable getFirstDefaultWitnessTableInModule() const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE static OptionalBridgedDefaultWitnessTable getNextDefaultWitnessTableInModule(
                                                                                  BridgedDefaultWitnessTable table);
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE OptionalBridgedFunction lookupFunction(BridgedStringRef name) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE OptionalBridgedFunction loadFunction(BridgedStringRef name,
                                                                          bool loadCalleesRecursively) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE void loadFunction(BridgedFunction function, bool loadCalleesRecursively) const;
  SWIFT_IMPORT_UNSAFE OptionalBridgedFunction lookupStdlibFunction(BridgedStringRef name) const;
  SWIFT_IMPORT_UNSAFE OptionalBridgedFunction lookUpNominalDeinitFunction(BridgedNominalTypeDecl nominal) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedSubstitutionMap getContextSubstitutionMap(BridgedType type) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedType getBuiltinIntegerType(SwiftInt bitWidth) const;
  SWIFT_IMPORT_UNSAFE BridgedFunction createEmptyFunction(BridgedStringRef name,
                                                          const BridgedParameterInfo * _Nullable bridgedParams,
                                                          SwiftInt paramCount,
                                                          bool hasSelfParam,
                                                          BridgedFunction fromFunc) const;
  void moveFunctionBody(BridgedFunction sourceFunc, BridgedFunction destFunc) const;

  // Passmanager housekeeping

  BRIDGED_INLINE void beginTransformFunction(BridgedFunction function) const;
  BRIDGED_INLINE void endTransformFunction() const;
  BRIDGED_INLINE bool continueWithNextSubpassRun(OptionalBridgedInstruction inst) const;

  // SSAUpdater

  BRIDGED_INLINE void
  SSAUpdater_initialize(BridgedFunction function, BridgedType type,
                        BridgedValue::Ownership ownership) const;
  BRIDGED_INLINE void SSAUpdater_addAvailableValue(BridgedBasicBlock block, BridgedValue value) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedValue SSAUpdater_getValueAtEndOfBlock(BridgedBasicBlock block) const;
  SWIFT_IMPORT_UNSAFE BRIDGED_INLINE BridgedValue SSAUpdater_getValueInMiddleOfBlock(BridgedBasicBlock block) const;

  // Options

  enum class AssertConfiguration {
    Debug = 0,
    Release = 1,
    Unchecked = 2
  };

  BRIDGED_INLINE bool enableStackProtection() const;
  BRIDGED_INLINE bool hasFeature(BridgedFeature feature) const;
  BRIDGED_INLINE bool enableMoveInoutStackProtection() const;
  BRIDGED_INLINE AssertConfiguration getAssertConfiguration() const;
  bool enableSimplificationFor(BridgedInstruction inst) const;
};

bool FullApplySite_canInline(BridgedInstruction apply);

enum class BridgedDynamicCastResult {
  willSucceed,
  maySucceed,
  willFail
};

BridgedDynamicCastResult classifyDynamicCastBridged(BridgedType sourceTy, BridgedType destTy,
                                                    BridgedFunction function,
                                                    bool sourceTypeIsExact);

BridgedDynamicCastResult classifyDynamicCastBridged(BridgedInstruction inst);

//===----------------------------------------------------------------------===//
//                          Pass registration
//===----------------------------------------------------------------------===//

struct BridgedFunctionPassCtxt {
  BridgedFunction function;
  BridgedPassContext passContext;
} ;

struct BridgedInstructionPassCtxt {
  BridgedInstruction instruction;
  BridgedPassContext passContext;
};

typedef void (* _Nonnull BridgedModulePassRunFn)(BridgedPassContext);
typedef void (* _Nonnull BridgedFunctionPassRunFn)(BridgedFunctionPassCtxt);
typedef void (* _Nonnull BridgedInstructionPassRunFn)(BridgedInstructionPassCtxt);

void SILPassManager_registerModulePass(BridgedStringRef name,
                                       BridgedModulePassRunFn runFn);
void SILPassManager_registerFunctionPass(BridgedStringRef name,
                                         BridgedFunctionPassRunFn runFn);
void SILCombine_registerInstructionPass(BridgedStringRef instClassName,
                                        BridgedInstructionPassRunFn runFn);

#ifndef PURE_BRIDGING_MODE
// In _not_ PURE_BRIDGING_MODE, briding functions are inlined and therefore inluded in the header file.
#include "OptimizerBridgingImpl.h"
#endif

SWIFT_END_NULLABILITY_ANNOTATIONS

#endif
