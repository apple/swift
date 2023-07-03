//===------------- Outliner.cpp - Outlining Transformations ---------------===//
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

#define DEBUG_TYPE "sil-outliner"

#include "swift/AST/ASTMangler.h"
#include "swift/AST/Module.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Types.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

llvm::cl::opt<std::string> DumpFuncsBeforeOutliner(
    "sil-dump-functions-before-outliner", llvm::cl::init(""),
    llvm::cl::desc(
        "Break before running each function pass on a particular function"));

namespace {

class OutlinerMangler : public Mangle::ASTMangler {
  /// The kind of method bridged.
  enum MethodKind : unsigned {
    BridgedProperty,
    BridgedProperty_Consuming,
    BridgedPropertyAddress,
    BridgedMethod,
  };

  llvm::BitVector *IsParameterBridged;
  llvm::BitVector *IsParameterGuaranteed;
  SILDeclRef MethodDecl;
  MethodKind Kind;
  bool IsReturnBridged;

public:
  /// Create an mangler for an outlined bridged method.
  OutlinerMangler(SILDeclRef Method, llvm::BitVector *ParameterBridged,
                  llvm::BitVector *IsParameterGuaranteed, bool ReturnBridged)
      : IsParameterBridged(ParameterBridged),
        IsParameterGuaranteed(IsParameterGuaranteed), MethodDecl(Method),
        Kind(BridgedMethod), IsReturnBridged(ReturnBridged) {}

  /// Create an mangler for an outlined bridged property.
  OutlinerMangler(SILDeclRef Method, bool IsAddress, bool ConsumesValue)
      : IsParameterBridged(nullptr), IsParameterGuaranteed(nullptr),
        MethodDecl(Method),
        Kind(IsAddress ? BridgedPropertyAddress
                       : (ConsumesValue ? BridgedProperty_Consuming
                                        : BridgedProperty)),
        IsReturnBridged(true) {}

  std::string mangle();

private:
  char getMethodKindMangling() {
    switch (Kind) {
    case BridgedProperty:
      return 'p';
    case BridgedProperty_Consuming:
      return 'o';
    case BridgedPropertyAddress:
      return 'a';
    case BridgedMethod:
      return 'm';
    }
    llvm_unreachable("unhandled kind");
  }
};
} // end anonymous namespace.

std::string OutlinerMangler::mangle() {
  beginManglingWithoutPrefix();

  appendOperator(MethodDecl.mangle());

  llvm::SmallString<128> Buffer;
  llvm::raw_svector_ostream Out(Buffer);

  Out << getMethodKindMangling();
  if (IsParameterBridged) {
    for (unsigned Idx = 0, E = IsParameterBridged->size(); Idx != E; ++Idx) {
      Out << (IsParameterBridged->test(Idx) ? 'b' : 'n');
      // NOTE: We must keep owned as having nothing here to preserve ABI since
      // mangling is part of ABI.
      Out << (IsParameterGuaranteed->test(Idx) ? "g" : "");
    }
  }
  Out << (IsReturnBridged ? 'b' : 'n');
  Out << '_';

  appendOperator("Te", Buffer);
  return finalize();
}

namespace {

class OutlinePattern {
protected:
  SILOptFunctionBuilder &FuncBuilder;
  InstModCallbacks callbacks;
  DeadEndBlocks *deBlocks;

public:
  OutlinePattern(SILOptFunctionBuilder &FuncBuilder,
                 InstModCallbacks callbacks,
                 DeadEndBlocks *deBlocks)
      : FuncBuilder(FuncBuilder), callbacks(callbacks), deBlocks(deBlocks) {}

  /// Match the instruction sequence.
  virtual bool matchInstSequence(SILBasicBlock::iterator I) = 0;

  /// Outline the matched instruction sequence.
  ///
  /// If a new outlined function is created return the function. If the outlined
  /// function already existed return null.
  /// Returns the last instruction of the matched sequence after the
  /// replacement.
  virtual std::pair<SILFunction *, SILBasicBlock::iterator>
  outline(SILModule &M) = 0;

  virtual std::string getOutlinedFunctionName() = 0;

  virtual ~OutlinePattern() {}
};

/// Get the bridgeToObjectiveC witness for the type.
static SILDeclRef getBridgeToObjectiveC(CanType NativeType,
                                        ModuleDecl *SwiftModule) {
  auto &Ctx = SwiftModule->getASTContext();
  auto Proto = Ctx.getProtocol(KnownProtocolKind::ObjectiveCBridgeable);
  if (!Proto)
    return SILDeclRef();
  auto ConformanceRef =
      SwiftModule->lookupConformance(NativeType, Proto);
  if (ConformanceRef.isInvalid())
    return SILDeclRef();

  auto Conformance = ConformanceRef.getConcrete();
  // bridgeToObjectiveC
  DeclName Name(Ctx, Ctx.Id_bridgeToObjectiveC, llvm::ArrayRef<Identifier>());
  auto *Requirement = dyn_cast_or_null<FuncDecl>(
    Proto->getSingleRequirement(Name));
  if (!Requirement)
    return SILDeclRef();

  auto Witness = Conformance->getWitnessDecl(Requirement);
  return SILDeclRef(Witness);
}

/// Get the _unconditionallyBridgeFromObjectiveC witness for the type.
SILDeclRef getBridgeFromObjectiveC(CanType NativeType,
                                   ModuleDecl *SwiftModule) {
  auto &Ctx = SwiftModule->getASTContext();
  auto Proto = Ctx.getProtocol(KnownProtocolKind::ObjectiveCBridgeable);
  if (!Proto)
    return SILDeclRef();
  auto ConformanceRef =
      SwiftModule->lookupConformance(NativeType, Proto);
  if (ConformanceRef.isInvalid())
    return SILDeclRef();
  auto Conformance = ConformanceRef.getConcrete();
  // _unconditionallyBridgeFromObjectiveC
  DeclName Name(Ctx, Ctx.getIdentifier("_unconditionallyBridgeFromObjectiveC"),
                llvm::makeArrayRef(Identifier()));
  auto *Requirement = dyn_cast_or_null<FuncDecl>(
      Proto->getSingleRequirement(Name));
  if (!Requirement)
    return SILDeclRef();

  auto Witness = Conformance->getWitnessDecl(Requirement);
  return SILDeclRef(Witness);
}

struct SwitchInfo {
  SwitchEnumInst *SwitchEnum = nullptr;
  SILBasicBlock *SomeBB = nullptr;
  SILBasicBlock *NoneBB = nullptr;
  BranchInst *Br = nullptr;
};

/// Pattern for a bridged property call.
///
///  bb7:
///    %30 = unchecked_take_enum_data_addr %19 : $*Optional<UITextField>, #Optional.some!enumelt
///    %31 = load %30 : $*UITextField
///    strong_retain %31 : $UITextField
///    %33 = objc_method %31 : $UITextField, #UITextField.text!getter.foreign : (UITextField) -> () -> String?, $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
///    %34 = apply %33(%31) : $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
///    switch_enum %34 : $Optional<NSString>, case #Optional.some!enumelt: bb8, case #Optional.none!enumelt: bb9
///
///  bb8(%36 : $NSString):
///    // function_ref static String._unconditionallyBridgeFromObjectiveC(_:)
///    %37 = function_ref @$SSS10FoundationE36_unconditionallyBridgeFromObjectiveCSSSo8NSStringCSgFZ : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
///    %38 = enum $Optional<NSString>, #Optional.some!enumelt, %36 : $NSString
///    %39 = metatype $@thin String.Type
///    %40 = apply %37(%38, %39) : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
///    %41 = enum $Optional<String>, #Optional.some!enumelt, %40 : $String
///    br bb10(%41 : $Optional<String>)
///
///  bb9:
///    %43 = enum $Optional<String>, #Optional.none!enumelt
///    br bb10(%43 : $Optional<String>)
///
///  bb10(%45 : $Optional<String>):
class BridgedProperty : public OutlinePattern {
  std::string OutlinedName;
  SingleValueInstruction *FirstInst; // A load or class_method
  SILBasicBlock *StartBB;
  SwitchInfo switchInfo;
  ObjCMethodInst *ObjCMethod;
  SILInstruction *Release;
  ApplyInst *PropApply;
  SILInstruction *UnpairedRelease; // A release_value | destroy_value following
                                   // the apply which isn't paired to
                                   // load [copy] | strong_retain first value.

public:
  bool matchInstSequence(SILBasicBlock::iterator I) override;

  std::pair<SILFunction *, SILBasicBlock::iterator>
  outline(SILModule &M) override;

  BridgedProperty(SILOptFunctionBuilder &FuncBuilder,
                  InstModCallbacks callbacks,
                  DeadEndBlocks *deBlocks)
      : OutlinePattern(FuncBuilder, callbacks, deBlocks) {
    clearState();
  }

  BridgedProperty(const BridgedProperty&) = delete;
  BridgedProperty& operator=(const BridgedProperty&) = delete;

  virtual ~BridgedProperty() {}

  std::string getOutlinedFunctionName() override;

private:
  bool matchMethodCall(SILBasicBlock::iterator, LoadInst *);
  CanSILFunctionType getOutlinedFunctionType(SILModule &M);
  void clearState();
};
}

void BridgedProperty::clearState() {
    FirstInst = nullptr;
    StartBB = nullptr;
    switchInfo = SwitchInfo();
    ObjCMethod = nullptr;
    Release = nullptr;
    PropApply = nullptr;
    UnpairedRelease = nullptr;
    OutlinedName.clear();
}

std::string BridgedProperty::getOutlinedFunctionName() {
  if (OutlinedName.empty()) {
    OutlinerMangler Mangler(ObjCMethod->getMember(), isa<LoadInst>(FirstInst),
                            UnpairedRelease);
    OutlinedName = Mangler.mangle();
  }
  return OutlinedName;
}

/// Returns the outlined function type.
///
/// This depends on the first instruction we matched. Either we matched a load
/// or we started the match at the class method instruction.
///
/// load %30 : *UITextField:
///   (@in_guaranteed InstanceType) -> (@owned Optional<BridgedInstanceType>)
/// objc_method %31 : UITextField
///   (@unowned InstanceType) -> (@owned Optional<BridgedInstanceType>)
///
CanSILFunctionType BridgedProperty::getOutlinedFunctionType(SILModule &M) {
  SmallVector<SILParameterInfo, 4> Parameters;
  if (auto *Load = dyn_cast<LoadInst>(FirstInst))
    Parameters.push_back(
      SILParameterInfo(Load->getType().getASTType(),
                       ParameterConvention::Indirect_In_Guaranteed));
  else
    Parameters.push_back(SILParameterInfo(
        cast<ObjCMethodInst>(FirstInst)->getOperand()->getType().getASTType(),
        UnpairedRelease ? ParameterConvention::Direct_Owned
                        : ParameterConvention::Direct_Unowned));
  SmallVector<SILResultInfo, 4> Results;

  Results.push_back(SILResultInfo(
                      switchInfo.Br->getArg(0)->getType().getASTType(),
                      ResultConvention::Owned));
  auto ExtInfo = SILFunctionType::ExtInfo::getThin();
  auto FunctionType = SILFunctionType::get(
      nullptr, ExtInfo, SILCoroutineKind::None,
      ParameterConvention::Direct_Unowned, Parameters, /*yields*/ {}, Results,
      llvm::None, SubstitutionMap(), SubstitutionMap(), M.getASTContext());
  return FunctionType;
}

static void eraseBlock(SILBasicBlock *block) {
  for (SILInstruction &inst : *block) {
    inst.replaceAllUsesOfAllResultsWithUndef();
  }
  block->eraseFromParent();
}

std::pair<SILFunction *, SILBasicBlock::iterator>
BridgedProperty::outline(SILModule &M) {
  // Get the function type.
  auto FunctionType = getOutlinedFunctionType(M);

  std::string nameTmp = getOutlinedFunctionName();
  auto name = M.allocateCopy(nameTmp);

  auto *Fun = FuncBuilder.getOrCreateFunction(
      ObjCMethod->getLoc(), name, SILLinkage::Shared, FunctionType, IsNotBare,
      IsNotTransparent, IsSerialized, IsNotDynamic, IsNotDistributed,
      IsNotRuntimeAccessible);
  bool NeedsDefinition = Fun->empty();

  if (Release) {
    // Move the release after the call.
    Release->moveBefore(StartBB->getTerminator());
  }

  //     [StartBB]
  //    /        \
  // [NoneBB]  [SomeBB]
  //   \          /
  //   [OldMergeBB]
  //
  //   Split to:
  //
  //      [StartBB]
  //          |
  //   [OutlinedEntryBB]   }
  //    /        \         }
  // [NoneBB]  [SomeBB]    } outlined
  //   \          /        }
  //   [OldMergeBB]        }
  //       |
  //   [NewTailBB]
  //
  auto *OutlinedEntryBB = StartBB->split(SILBasicBlock::iterator(FirstInst));
  auto *OldMergeBB = switchInfo.Br->getDestBB();
  auto *NewTailBB = OldMergeBB->split(OldMergeBB->begin());
  if (deBlocks) {
    deBlocks->updateForNewBlock(NewTailBB);
  }
  // Call the outlined function.
  {
    SILBuilder Builder(StartBB);
    auto Loc = FirstInst->getLoc();
    SILValue FunRef(Builder.createFunctionRef(Loc, Fun));
    SILValue Apply(
        Builder.createApply(Loc, FunRef, SubstitutionMap(),
                            {FirstInst->getOperand(0)}));
    Builder.createBranch(Loc, NewTailBB);
    OldMergeBB->getArgument(0)->replaceAllUsesWith(Apply);
  }

  if (!NeedsDefinition) {
    // Delete the outlined instructions/blocks.
    if (Release)
      Release->eraseFromParent();
    eraseBlock(OutlinedEntryBB);
    eraseBlock(switchInfo.NoneBB);
    eraseBlock(switchInfo.SomeBB);
    eraseBlock(OldMergeBB);
    return std::make_pair(nullptr, std::prev(StartBB->end()));
  }

  if (!OutlinedEntryBB->getParent()->hasOwnership())
    Fun->setOwnershipEliminated();

  Fun->setInlineStrategy(NoInline);

  // Move the blocks into the new function.
  Fun->moveBlockFromOtherFunction(OldMergeBB, Fun->begin());
  Fun->moveBlockFromOtherFunction(switchInfo.NoneBB, Fun->begin());
  Fun->moveBlockFromOtherFunction(switchInfo.SomeBB, Fun->begin());
  Fun->moveBlockFromOtherFunction(OutlinedEntryBB, Fun->begin());

  // Create the function argument and return.
  auto *Load = dyn_cast<LoadInst>(FirstInst);
  SILBuilder Builder(FirstInst);
  if (Load) {
    OutlinedEntryBB->createFunctionArgument(Load->getOperand()->getType());
    auto *NewLoad =
        Builder.createLoad(Load->getLoc(), OutlinedEntryBB->getArgument(0),
                           Load->getOwnershipQualifier());
    Load->replaceAllUsesWith(NewLoad);
    Load->eraseFromParent();
  } else {
    OutlinedEntryBB->createFunctionArgument(
        FirstInst->getOperand(0)->getType());
    auto *Arg = OutlinedEntryBB->getArgument(0);
    FirstInst->setOperand(0, Arg);
    if (UnpairedRelease)
      UnpairedRelease->setOperand(0, Arg);
    PropApply->setArgument(0, Arg);
  }
  Builder.setInsertionPoint(OldMergeBB);
  Builder.createReturn(ObjCMethod->getLoc(), OldMergeBB->getArgument(0));
  return std::make_pair(Fun, std::prev(StartBB->end()));
}

#define ADVANCE_ITERATOR_OR_RETURN_FALSE(It)                                   \
  do {                                                                         \
    if (It->getParent()->end() == ++It)                                        \
      return false;                                                            \
  } while (0);

static bool matchSwitch(SwitchInfo &SI, SILInstruction *Inst,
                        SILValue SwitchOperand) {
  auto *SwitchEnum = dyn_cast<SwitchEnumInst>(Inst);
  if (!SwitchEnum || SwitchEnum->getNumCases() != 2 ||
      SwitchEnum->getOperand() != SwitchOperand)
    return false;

  auto *SwitchBB = SwitchEnum->getParent();
  SILBasicBlock *SomeBB = SwitchEnum->getCase(0).second;
  SILBasicBlock *NoneBB = SwitchEnum->getCase(1).second;
  if (NoneBB->getSinglePredecessorBlock() != SwitchBB)
    return false;
  if (SomeBB->getSinglePredecessorBlock() != SwitchBB)
    return false;
  if (NoneBB->args_size() == 1)
    std::swap(NoneBB, SomeBB);
  if (SomeBB->args_size() != 1 || NoneBB->args_size() != 0)
    return false;

  // bb9:
  // %43 = enum $Optional<String>, #Optional.none!enumelt
  auto It = NoneBB->begin();
  auto *NoneEnum = dyn_cast<EnumInst>(It);
  if (!NoneEnum || NoneEnum->hasOperand() || !NoneEnum->hasOneUse())
    return false;

  // br bb10(%43 : $Optional<String>)
  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  auto *Br1 = dyn_cast<BranchInst>(It);
  if (!Br1 || Br1->getNumArgs() != 1 || Br1->getArg(0) != NoneEnum)
    return false;
  auto *MergeBB = Br1->getDestBB();

  // bb8(%36 : $NSString):
  It = SomeBB->begin();
  auto *SomeBBArg = SomeBB->getArgument(0);
  if (!SomeBBArg->hasOneUse())
    return false;

  // %37 = function_ref @$SSS10FoundationE36_unconditionallyBridgeFromObjectiveCSSSo8NSStringCSgFZ : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
  auto *FunRef = dyn_cast<FunctionRefInst>(It);
  if (!FunRef || !FunRef->hasOneUse())
    return false;

  // %38 = enum $Optional<NSString>, #Optional.some!enumelt, %36 : $NSString
  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  auto *SomeEnum = dyn_cast<EnumInst>(It);
  if (!SomeEnum || !SomeEnum->hasOperand() || SomeEnum->getOperand() != SomeBBArg)
    return false;
  size_t numSomeEnumUses = std::distance(SomeEnum->use_begin(), SomeEnum->use_end());
  if (numSomeEnumUses > 2)
    return false;

  // %39 = metatype $@thin String.Type
  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  auto *Metatype = dyn_cast<MetatypeInst>(It);
  if (!Metatype || !Metatype->hasOneUse())
    return false;

  // %40 = apply %37(%38, %39) : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  auto *Apply = dyn_cast<ApplyInst>(It);
  if (!Apply || !Apply->hasOneUse() || Apply->getCallee() != FunRef ||
      Apply->getNumArguments() != 2 || Apply->getArgument(0) != SomeEnum ||
      Apply->getArgument(1) != Metatype ||
      Apply->getSubstCalleeType()->getNumResults() != 1)
    return false;
  if (Apply->getSubstCalleeType()->getSingleResult().getConvention() !=
      ResultConvention::Owned)
    return false;

  // Check that we call the _unconditionallyBridgeFromObjectiveC witness.
  auto NativeType = Apply->getType().getASTType();
  auto *BridgeFun = FunRef->getReferencedFunction();
  auto *SwiftModule = BridgeFun->getModule().getSwiftModule();
  // Not every type conforms to the ObjectiveCBridgeable protocol in such a case
  // getBridgeFromObjectiveC returns SILDeclRef().
  auto bridgeWitness = getBridgeFromObjectiveC(NativeType, SwiftModule);
  if (bridgeWitness == SILDeclRef() ||
      BridgeFun->getName() != bridgeWitness.mangle())
    return false;

  // %41 = enum $Optional<String>, #Optional.some!enumelt, %40 : $String
  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  auto *Enum3 = dyn_cast<EnumInst>(It);
  if (!Enum3 || !Enum3->hasOneUse() || !Enum3->hasOperand() ||
      Enum3->getOperand() != Apply)
    return false;

  if (numSomeEnumUses == 2) {
    // [release_value | destroy_value] %38 : $Optional<NSString>
    ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
    bool hasOwnership = It->getFunction()->hasOwnership();
    if (hasOwnership) {
      auto *DVI = dyn_cast<DestroyValueInst>(It);
      if (!DVI || DVI->getOperand() != SomeEnum)
        return false;
    } else {
      auto *RVI = dyn_cast<ReleaseValueInst>(It);
      if (!RVI || RVI->getOperand() != SomeEnum)
        return false;
    }
  }

  // br bb10(%41 : $Optional<String>)
  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  auto *Br = dyn_cast<BranchInst>(It);
  if (!Br || Br->getDestBB() != MergeBB || Br->getNumArgs() != 1 ||
      Br->getArg(0) != Enum3)
    return false;

  SI.SwitchEnum = SwitchEnum;
  SI.SomeBB = SomeBB;
  SI.NoneBB = NoneBB;
  SI.Br = Br;
  return true;
}

bool BridgedProperty::matchMethodCall(SILBasicBlock::iterator It,
                                      LoadInst *Load) {
  // Matches:
  //    %33 = objc_method %31 : $UITextField, #UITextField.text!getter.foreign : (UITextField) -> () -> String?, $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
  //    %34 = apply %33(%31) : $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
  //    switch_enum %34 : $Optional<NSString>, case #Optional.some!enumelt: bb8, case #Optional.none!enumelt: bb9
  //
  //  bb8(%36 : $NSString):
  //    %37 = function_ref @$SSS10FoundationE36_unconditionallyBridgeFromObjectiveCSSSo8NSStringCSgFZ : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
  //    %38 = enum $Optional<NSString>, #Optional.some!enumelt, %36 : $NSString
  //    %39 = metatype $@thin String.Type
  //    %40 = apply %37(%38, %39) : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
  //    %41 = enum $Optional<String>, #Optional.some!enumelt, %40 : $String
  //    br bb10(%41 : $Optional<String>)
  //
  //  bb9:
  //    %43 = enum $Optional<String>, #Optional.none!enumelt
  //    br bb10(%43 : $Optional<String>)
  //
  //  bb10(%45 : $Optional<String>):
  //

  // %33 = objc_method %31 : $UITextField, #UITextField.text!getter.foreign
  ObjCMethod = dyn_cast<ObjCMethodInst>(It);
  SILValue Instance =
      FirstInst != ObjCMethod ? FirstInst : ObjCMethod->getOperand();
  if (!ObjCMethod || !ObjCMethod->hasOneUse() ||
      ObjCMethod->getOperand() != Instance ||
      ObjCMethod->getFunction()->getLoweredFunctionType()->isPolymorphic() ||
      ObjCMethod->getType().castTo<SILFunctionType>()->isPolymorphic() ||
      ObjCMethod->getType().castTo<SILFunctionType>()->hasOpenedExistential())
    return false;

  // %34 = apply %33(%31) : $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  PropApply = dyn_cast<ApplyInst>(It);
  if (!PropApply || PropApply->getCallee() != ObjCMethod ||
      PropApply->getNumArguments() != 1 ||
      PropApply->getArgument(0) != Instance || !PropApply->hasOneUse())
    return false;

  if (Load) {
    // In OSSA, there will be a destroy_value matching the earlier load [copy].
    // In non-ossa, there will be a release matching the earlier retain. The
    // only user of the retained value is the unowned objective-c method
    // consumer.
    unsigned NumUses = 0;
    Release = nullptr;
    bool hasOwnership = Load->getFunction()->hasOwnership();
    for (auto *Use : Load->getUses()) {
      ++NumUses;
      SILInstruction *R;
      if (hasOwnership) {
        R = dyn_cast<DestroyValueInst>(Use->getUser());
      } else {
        R = dyn_cast<StrongReleaseInst>(Use->getUser());
      }
      if (R) {
        if (!Release) {
          Release = R;
        } else {
          Release = nullptr;
          break;
        }
      }
    }
    if (!Release)
      return false;
    if (hasOwnership) {
      if (NumUses != 3)
        return false;
    } else {
      if (NumUses != 4)
        return false;
    }
    ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
    if (Release != &*It)
      return false;
  }

  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  if (auto *dvi = dyn_cast<DestroyValueInst>(*&It)) {
    if (Load)
      return false;
    if (dvi->getOperand() != Instance)
      return false;
    UnpairedRelease = dvi;
    ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  }

  // Don't outline in the outlined function.
  if (ObjCMethod->getFunction()->getName().equals(getOutlinedFunctionName()))
    return false;

  // switch_enum %34 : $Optional<NSString>, case #Optional.some!enumelt: bb8,
  // case #Optional.none!enumelt: bb9
  return matchSwitch(switchInfo, &*It, PropApply);
}

bool BridgedProperty::matchInstSequence(SILBasicBlock::iterator It) {
  // Matches:
  // [ optionally:
  //    %31 = load %30 : $*UITextField or %31 = load [copy] %30 : $*UITextField
  //    strong_retain %31 : $UITextField
  // ]
  //    %33 = objc_method %31 : $UITextField, #UITextField.text!getter.foreign : (UITextField) -> () -> String?, $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
  //    %34 = apply %33(%31) : $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
  //    switch_enum %34 : $Optional<NSString>, case #Optional.some!enumelt: bb8, case #Optional.none!enumelt: bb9
  //
  //  bb8(%36 : $NSString):
  //    %37 = function_ref @$SSS10FoundationE36_unconditionallyBridgeFromObjectiveCSSSo8NSStringCSgFZ : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
  //    %38 = enum $Optional<NSString>, #Optional.some!enumelt, %36 : $NSString
  //    %39 = metatype $@thin String.Type
  //    %40 = apply %37(%38, %39) : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
  //    %41 = enum $Optional<String>, #Optional.some!enumelt, %40 : $String
  //    br bb10(%41 : $Optional<String>)
  //
  //  bb9:
  //    %43 = enum $Optional<String>, #Optional.none!enumelt
  //    br bb10(%43 : $Optional<String>)
  //
  //  bb10(%45 : $Optional<String>):

  clearState();

  // %31 = load %30 : $*UITextField
  auto *Load = dyn_cast<LoadInst>(It);

  // Otherwise, trying matching from the method call.
  if (!Load) {
    // Try to match without the load/strong_retain prefix.
    auto *CMI = dyn_cast<ObjCMethodInst>(It);
    if (!CMI || CMI->getFunction()->getLoweredFunctionType()->isPolymorphic() ||
        CMI->getType().castTo<SILFunctionType>()->isPolymorphic() ||
        CMI->getType().castTo<SILFunctionType>()->hasOpenedExistential())
      return false;
    FirstInst = CMI;
  } else
    FirstInst = Load;

  StartBB = FirstInst->getParent();

  if (Load) {
    if (Load->getFunction()->hasOwnership()) {
      if (Load->getOwnershipQualifier() != LoadOwnershipQualifier::Copy)
        return false;
      ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
    } else {
      // strong_retain %31 : $UITextField
      ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
      auto *Retain = dyn_cast<StrongRetainInst>(It);
      if (!Retain || Retain->getOperand() != Load)
        return false;
      ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
    }
  }

  if (!matchMethodCall(It, Load))
    return false;

  return true;
}


namespace {

/// Match a bridged argument.
///
///
/// %15 = function_ref @$SSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF
/// %16 = apply %15(%14) :
///         $@convention(method) (@guaranteed String) -> @owned NSString
/// %17 = enum $Optional<NSString>, #Optional.some!enumelt, %16 : $NSString
/// release_value %14 : $String
///
/// apply %objcMethod(%17, ...) : $@convention(objc_method) (Optional<NSString> ...) ->
/// release_value %17 : $Optional<NSString>
///
/// NOTE: If release_value %14 is found, our outlined function will have an
/// owned convention for self. Otherwise, if we do not find it, we will have a
/// guaranteed one.
class BridgedArgument {
public:
  FunctionRefInst *BridgeFun;
  ApplyInst *BridgeCall;
  EnumInst *OptionalResult;
  SILValue BridgedValue;
  SILInstruction *ReleaseAfterBridge;
  SILInstruction *ReleaseArgAfterCall;
  unsigned Idx = 0;

  // Matched bridged argument.
  BridgedArgument(unsigned Idx, FunctionRefInst *F, ApplyInst *A, EnumInst *E,
                  SILInstruction *R0, SILInstruction *R1)
      : BridgeFun(F), BridgeCall(A), OptionalResult(E),
        BridgedValue(FullApplySite(A).getSelfArgument()),
        ReleaseAfterBridge(R0), ReleaseArgAfterCall(R1), Idx(Idx) {
    assert(!R0 || isa<ReleaseValueInst>(R0) || isa<DestroyValueInst>(R0));
  }

  /// Invalid argument constructor.
  BridgedArgument()
      : BridgeFun(nullptr), BridgeCall(nullptr), OptionalResult(nullptr),
        ReleaseAfterBridge(nullptr), ReleaseArgAfterCall(nullptr), Idx(0) {}

  static BridgedArgument match(unsigned ArgIdx, SILValue Arg, ApplyInst *AI);

  operator bool() const { return BridgeFun != nullptr; }
  SILValue bridgedValue() { return BridgedValue; }

  bool isGuaranteed() const { return ReleaseAfterBridge == nullptr; }
  ParameterConvention getConvention() const {
    if (isGuaranteed())
      return ParameterConvention::Direct_Guaranteed;
    return ParameterConvention::Direct_Owned;
  }

  void eraseFromParent();

  /// Move the bridged argument sequence to the bridged call block.
  /// Precondition: The bridged call has already been moved to the outlined
  /// function.
  void transferTo(SILValue BridgedValueFunArg, ApplyInst *BridgedCall);
};
}

void BridgedArgument::transferTo(SILValue BridgedValue,
                                 ApplyInst *BridgedCall) {
  assert(BridgedCall->getParent() != BridgeFun->getParent());
  // Move the instructions to the bridged call that we have already moved and
  // update the uses of the bridge value by the function argument value passed
  // to this function.
  auto *DestBB = BridgedCall->getParent();
  DestBB->moveTo(SILBasicBlock::iterator(BridgedCall), BridgeFun);
  DestBB->moveTo(SILBasicBlock::iterator(BridgedCall), BridgeCall);
  BridgeCall->setArgument(0, BridgedValue);
  DestBB->moveTo(SILBasicBlock::iterator(BridgedCall), OptionalResult);
  if (ReleaseAfterBridge) {
    DestBB->moveTo(SILBasicBlock::iterator(BridgedCall), ReleaseAfterBridge);
    ReleaseAfterBridge->setOperand(0, BridgedValue);
  }
  auto AfterCall = std::next(SILBasicBlock::iterator(BridgedCall));
  DestBB->moveTo(SILBasicBlock::iterator(AfterCall), ReleaseArgAfterCall);
}

void BridgedArgument::eraseFromParent() {
  if (ReleaseAfterBridge)
    ReleaseAfterBridge->eraseFromParent();
  ReleaseArgAfterCall->eraseFromParent();
  OptionalResult->eraseFromParent();
  BridgeCall->eraseFromParent();
  BridgeFun->eraseFromParent();
}

static SILInstruction *findReleaseOf(SILValue releasedValue,
                                     SILBasicBlock::iterator from,
                                     SILBasicBlock::iterator to) {
  bool hasOwnership = releasedValue->getFunction()->hasOwnership();
  while (from != to) {
    if (hasOwnership) {
      auto destroy = dyn_cast<DestroyValueInst>(&*from);
      if (destroy && destroy->getOperand() == releasedValue)
        return destroy;
    } else {
      auto release = dyn_cast<ReleaseValueInst>(&*from);
      if (release && release->getOperand() == releasedValue)
        return release;
    }
    ++from;
  }
  return nullptr;
}

BridgedArgument BridgedArgument::match(unsigned ArgIdx, SILValue Arg,
                                       ApplyInst *AI) {
  // Match
  // %15 = function_ref @$SSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF
  // %16 = apply %15(%14) : $@convention(method) (@guaranteed String) -> @owned NSString
  // %17 = enum $Optional<NSString>, #Optional.some!enumelt, %16 : $NSString
  // [release_value | destroy_value] %14 : $String
  // ...
  // apply %objcMethod(%17, ...) : $@convention(objc_method) (Optional<NSString>...) ->
  // release_value ...
  // [release_value | destroy_value] %17 : $Optional<NSString>
  //
  auto *Enum = dyn_cast<EnumInst>(Arg);
  if (!Enum || !Enum->hasOperand())
    return BridgedArgument();

  if (SILBasicBlock::iterator(Enum) == Enum->getParent()->begin())
    return BridgedArgument();
  auto *BridgeCall =
      dyn_cast<ApplyInst>(std::prev(SILBasicBlock::iterator(Enum)));
  if (!BridgeCall || BridgeCall->getNumArguments() != 1 ||
      Enum->getOperand() != BridgeCall || !BridgeCall->hasOneUse() ||
      !FullApplySite(BridgeCall).hasSelfArgument())
    return BridgedArgument();

  auto &selfArg = FullApplySite(BridgeCall).getSelfArgumentOperand();
  auto selfConvention =
      FullApplySite(BridgeCall).getArgumentConvention(selfArg);
  if (selfConvention != SILArgumentConvention::Direct_Guaranteed &&
      selfConvention != SILArgumentConvention::Direct_Owned)
    return BridgedArgument();

  auto BridgedValue = BridgeCall->getArgument(0);
  auto Next = std::next(SILBasicBlock::iterator(Enum));
  if (Next == Enum->getParent()->end())
    return BridgedArgument();

  // Make sure that if we have a bridged value release that it is on the bridged
  // value.
  if (Enum->getParent() != AI->getParent())
    return BridgedArgument();

  bool hasOwnership = AI->getFunction()->hasOwnership();
  auto *BridgedValueRelease =
      findReleaseOf(BridgedValue, std::next(SILBasicBlock::iterator(Enum)),
                    SILBasicBlock::iterator(AI));
  assert(!BridgedValueRelease ||
         (hasOwnership && isa<DestroyValueInst>(BridgedValueRelease)) ||
         isa<ReleaseValueInst>(BridgedValueRelease));
  if (BridgedValueRelease && BridgedValueRelease->getOperand(0) != BridgedValue)
    return BridgedArgument();

  if (SILBasicBlock::iterator(BridgeCall) == BridgeCall->getParent()->begin())
    return BridgedArgument();
  auto *FunRef =
      dyn_cast<FunctionRefInst>(std::prev(SILBasicBlock::iterator(BridgeCall)));
  if (!FunRef || !FunRef->hasOneUse() || BridgeCall->getCallee() != FunRef)
    return BridgedArgument();

  SILInstruction *ReleaseAfter = nullptr;
  for (auto *Use : Enum->getUses()) {
    if (Use->getUser() == AI)
      continue;

    // The enum must only have two uses the release and the apply.
    if (ReleaseAfter)
      return BridgedArgument();

    if (hasOwnership) {
      ReleaseAfter = dyn_cast<DestroyValueInst>(Use->getUser());
    } else {
      ReleaseAfter = dyn_cast<ReleaseValueInst>(Use->getUser());
    }
    if (!ReleaseAfter)
      return BridgedArgument();
  }

  // Make sure we are calling the actual bridge witness.
  auto NativeType = BridgedValue->getType().getASTType();
  auto *BridgeFun = FunRef->getReferencedFunction();
  auto *SwiftModule = BridgeFun->getModule().getSwiftModule();
  // Not every type conforms to the ObjectiveCBridgeable protocol in such a case
  // getBridgeToObjectiveC returns SILDeclRef().
  auto bridgeWitness = getBridgeToObjectiveC(NativeType, SwiftModule);
  if (bridgeWitness == SILDeclRef() ||
      BridgeFun->getName() != bridgeWitness.mangle())
    return BridgedArgument();

  return BridgedArgument(ArgIdx, FunRef, BridgeCall, Enum, BridgedValueRelease,
                         ReleaseAfter);
}

namespace {
// Match the return value bridging pattern.
//   switch_enum %20 : $Optional<NSString>, case #O.some: bb1, case #O.none: bb2
//
// bb1(%23 : $NSString):
//   %24 = function_ref @_unconditionallyBridgeFromObjectiveC
//   %25 = enum $Optional<NSString>, #Optional.some!enumelt, %23 : $NSString
//   %26 = metatype $@thin String.Type
//   %27 = apply %24(%25, %26)
//   %28 = enum $Optional<String>, #Optional.some!enumelt, %27 : $String
//   br bb3(%28 : $Optional<String>)
//
// bb2:
//   %30 = enum $Optional<String>, #Optional.none!enumelt
//   br bb3(%30 : $Optional<String>)
//
// bb3(%32 : $Optional<String>):
class BridgedReturn {
  DeadEndBlocks *deBlocks;
  SwitchInfo switchInfo;
public:
  BridgedReturn(DeadEndBlocks *deBlocks) : deBlocks(deBlocks) {}

  bool match(ApplyInst *BridgedCall) {
    switchInfo = SwitchInfo();
    auto *SwitchBB = BridgedCall->getParent();
    return matchSwitch(switchInfo, SwitchBB->getTerminator(), BridgedCall);
  }

  operator bool() { return switchInfo.SomeBB != nullptr; }

  CanType getReturnType() {
    return switchInfo.Br->getArg(0)->getType().getASTType();
  }

  /// Outline the return value bridging blocks.
  void outline(SILFunction *Fun, ApplyInst *NewOutlinedCall);
};
}

void BridgedReturn::outline(SILFunction *Fun, ApplyInst *NewOutlinedCall) {
// Outline the bridged return result blocks.
//   switch_enum %20 : $Optional<NSString>, case #O.some: bb1, case #O.none: bb2
//
// bb1(%23 : $NSString):
//   %24 = function_ref @$SSS10FoundationE36_unconditionallyBridgeFromObjectiveC
//   %25 = enum $Optional<NSString>, #Optional.some!enumelt, %23 : $NSString
//   %26 = metatype $@thin String.Type
//   %27 = apply %24(%25, %26)
//   %28 = enum $Optional<String>, #Optional.some!enumelt, %27 : $String
//   br bb3(%28 : $Optional<String>)
//
// bb2:
//   %30 = enum $Optional<String>, #Optional.none!enumelt
//   br bb3(%30 : $Optional<String>)
//
// bb3(%32 : $Optional<String>):

  auto *StartBB = switchInfo.SwitchEnum->getParent();
  auto *OutlinedEntryBB = StartBB->split(SILBasicBlock::iterator(switchInfo.SwitchEnum));
  auto *OldMergeBB = switchInfo.Br->getDestBB();
  auto *NewTailBB = OldMergeBB->split(OldMergeBB->begin());
  if (deBlocks) {
    deBlocks->updateForNewBlock(NewTailBB);
  }
  auto Loc = switchInfo.SwitchEnum->getLoc();

  {
    SILBuilder Builder(StartBB);
    Builder.createBranch(Loc, NewTailBB);
    OldMergeBB->getArgument(0)->replaceAllUsesWith(NewOutlinedCall);
  }

  // Outlined function already existed. Just delete instructions and wire up
  // blocks.
  if (!Fun) {
    eraseBlock(OutlinedEntryBB);
    eraseBlock(switchInfo.NoneBB);
    eraseBlock(switchInfo.SomeBB);
    eraseBlock(OldMergeBB);
    return;
  }

  // Move the blocks into the new function.
  assert(Fun->begin() != Fun->end() &&
         "The entry block must already have been created");
  SILBasicBlock *EntryBB = &*Fun->begin();
  Fun->moveBlockFromOtherFunction(OldMergeBB, Fun->begin());
  Fun->moveBlockAfter(OldMergeBB, EntryBB);
	auto InsertPt = SILFunction::iterator(OldMergeBB);
  Fun->moveBlockFromOtherFunction(OutlinedEntryBB, InsertPt);
  Fun->moveBlockFromOtherFunction(switchInfo.NoneBB, InsertPt);
  Fun->moveBlockFromOtherFunction(switchInfo.SomeBB, InsertPt);

	SILBuilder Builder (EntryBB);
  Builder.createBranch(Loc, OutlinedEntryBB);

  Builder.setInsertionPoint(OldMergeBB);
  Builder.createReturn(Loc, OldMergeBB->getArgument(0));
}

namespace {
class ObjCMethodCall : public OutlinePattern {
  ObjCMethodInst *ObjCMethod;
  ApplyInst *BridgedCall;
  SmallVector<BridgedArgument, 4> BridgedArguments;
  std::string OutlinedName;
  llvm::BitVector IsBridgedArgument;
  llvm::BitVector IsGuaranteedArgument;
  ::BridgedReturn BridgedReturn;
public:
  bool matchInstSequence(SILBasicBlock::iterator I) override;

  std::pair<SILFunction *, SILBasicBlock::iterator>
  outline(SILModule &M) override;

  ObjCMethodCall(SILOptFunctionBuilder &FuncBuilder,
                 InstModCallbacks callbacks,
                 DeadEndBlocks *deBlocks)
      : OutlinePattern(FuncBuilder, callbacks, deBlocks), BridgedReturn(deBlocks) {}
  ~ObjCMethodCall();

private:
  void clearState();
  std::string getOutlinedFunctionName() override;
  CanSILFunctionType getOutlinedFunctionType(SILModule &M);
};
}

ObjCMethodCall::~ObjCMethodCall() {
  clearState();
}

void ObjCMethodCall::clearState() {
  ObjCMethod = nullptr;
  BridgedCall = nullptr;
  BridgedArguments.clear();
  OutlinedName.clear();
  IsBridgedArgument.clear();
  IsGuaranteedArgument.clear();
}

std::pair<SILFunction *, SILBasicBlock::iterator>
ObjCMethodCall::outline(SILModule &M) {

  auto FunctionType = getOutlinedFunctionType(M);
  std::string nameTmp = getOutlinedFunctionName();
  auto name = M.allocateCopy(nameTmp);

  auto *Fun = FuncBuilder.getOrCreateFunction(
      ObjCMethod->getLoc(), name, SILLinkage::Shared, FunctionType, IsNotBare,
      IsNotTransparent, IsSerialized, IsNotDynamic, IsNotDistributed,
      IsNotRuntimeAccessible);
  bool NeedsDefinition = Fun->empty();

  // Call the outlined function.
  ApplyInst *OutlinedCall;
  {
    SILBuilder Builder(BridgedCall);

    auto Loc = BridgedCall->getLoc();
    SILValue FunRef(Builder.createFunctionRef(Loc, Fun));

    // Collect the arguments for the apply.
    SmallVector<SILValue, 8> Args;
    unsigned OrigSigIdx = 0;
    unsigned BridgedArgIdx = 0;
    for (auto Arg : BridgedCall->getArguments()) {
      if (BridgedArgIdx < BridgedArguments.size() &&
          BridgedArguments[BridgedArgIdx].Idx == OrigSigIdx) {
        auto bridgedArgValue = BridgedArguments[BridgedArgIdx].bridgedValue();
        if (bridgedArgValue->getOwnershipKind() == OwnershipKind::Guaranteed) {
          bridgedArgValue = makeGuaranteedValueAvailable(
              bridgedArgValue, BridgedCall, *deBlocks);
        }
        Args.push_back(bridgedArgValue);
        ++BridgedArgIdx;
      } else {
        // Otherwise, use the original type convention.
        Args.push_back(Arg);
      }
      ++OrigSigIdx;
    }
    OutlinedCall = Builder.createApply(Loc, FunRef, SubstitutionMap(), Args);
    if (!BridgedCall->use_empty() && !BridgedReturn)
      BridgedCall->replaceAllUsesWith(OutlinedCall);
  }

  // Outlined function already exists. Only need to delete basic blocks/instructions.
  if (!NeedsDefinition) {
		if (BridgedReturn)
      BridgedReturn.outline(nullptr, OutlinedCall);
    BridgedCall->eraseFromParent();
    ObjCMethod->eraseFromParent();
    // Remove bridged argument code.
    for (auto Arg : BridgedArguments)
      Arg.eraseFromParent();
    SILBasicBlock::iterator I(OutlinedCall);
    return std::make_pair(Fun, I);
  }

  if (!ObjCMethod->getFunction()->hasOwnership())
    Fun->setOwnershipEliminated();

  Fun->setInlineStrategy(NoInline);

  // Create the entry block.
  auto *EntryBB = Fun->createBasicBlock();

  // Move the bridged call.
  EntryBB->moveTo(EntryBB->end(), ObjCMethod);
  EntryBB->moveTo(EntryBB->end(), BridgedCall);

  // Create the arguments.
  unsigned OrigSigIdx = 0;
  unsigned BridgedArgIdx = 0;
  SILValue LastArg;
  for (auto Arg : BridgedCall->getArguments()) {
    if (BridgedArgIdx < BridgedArguments.size() &&
        BridgedArguments[BridgedArgIdx].Idx == OrigSigIdx) {
      auto &BridgedArg = BridgedArguments[BridgedArgIdx];
      auto *FunArg =
          EntryBB->createFunctionArgument(BridgedArg.bridgedValue()->getType());
      BridgedArg.transferTo(FunArg, BridgedCall);
      ++BridgedArgIdx;
    } else {
      auto *FunArg = EntryBB->createFunctionArgument(Arg->getType());
      BridgedCall->setArgument(OrigSigIdx, FunArg);
      LastArg = FunArg;
    }
    ++OrigSigIdx;
  }

  // Set the method lookup's target.
  ObjCMethod->setOperand(LastArg);

  // Create the return and optionally move the bridging code.
  if (!BridgedReturn) {
    SILBuilder Builder(EntryBB);
    Builder.createReturn(BridgedCall->getLoc(), BridgedCall);
  } else {
    BridgedReturn.outline(Fun, OutlinedCall);
  }

  SILBasicBlock::iterator I(OutlinedCall);
  return std::make_pair(Fun, I);
}

std::string ObjCMethodCall::getOutlinedFunctionName() {
  if (OutlinedName.empty()) {
    OutlinerMangler Mangler(ObjCMethod->getMember(), &IsBridgedArgument,
                            &IsGuaranteedArgument, BridgedReturn);
    OutlinedName = Mangler.mangle();
  }
  return OutlinedName;
}

bool ObjCMethodCall::matchInstSequence(SILBasicBlock::iterator I) {
  clearState();

  ObjCMethod = dyn_cast<ObjCMethodInst>(I);
  if (!ObjCMethod ||
      ObjCMethod->getFunction()->getLoweredFunctionType()->isPolymorphic() ||
      ObjCMethod->getType().castTo<SILFunctionType>()->isPolymorphic() ||
      ObjCMethod->getType().castTo<SILFunctionType>()->hasOpenedExistential())
    return false;

  auto *Use = ObjCMethod->getSingleUse();
  if (!Use)
    return false;
  BridgedCall = dyn_cast<ApplyInst>(Use->getUser());
  if (!BridgedCall ||
      (!BridgedCall->hasOneUse() && !BridgedCall->use_empty()) ||
      ObjCMethod->getParent() != BridgedCall->getParent())
    return false;

  // Collect bridged parameters.
  unsigned Idx = 0;
  IsBridgedArgument.resize(BridgedCall->getNumArguments(), false);
  IsGuaranteedArgument.resize(BridgedCall->getNumArguments(), false);
  for (auto &Param : BridgedCall->getArgumentOperands()) {
    unsigned CurIdx = Idx++;

    // Look for Optional<NSFoo> type.
    auto Ty = Param.get()->getType().getOptionalObjectType();
    if (!Ty)
      continue;

    // Can't handle AnyObject. The concrete class type can be different since we
    // are passing 'id'. To make this work we would have to mangle the type into
    // the function name.
    if (Ty.isAnyObject())
      continue;

    auto BridgedArg = BridgedArgument::match(CurIdx, Param.get(), BridgedCall);
    if (!BridgedArg)
      continue;

    BridgedArguments.push_back(BridgedArg);
    IsBridgedArgument.set(CurIdx);
    if (BridgedArg.isGuaranteed())
      IsGuaranteedArgument.set(CurIdx);
  }

  // Try to match a bridged return value.
  BridgedReturn.match(BridgedCall);

  // Don't outline inside the outlined function.
  auto OutlinedName = getOutlinedFunctionName();
  auto CurrentName = ObjCMethod->getFunction()->getName();
  if (CurrentName.equals(OutlinedName))
    return false;

  // Don't outline if we created an outlined function without the bridged result
  // from the outlined function with the bridged result (only the suffix is
  // different: MethodNameTem...n_ vs MethodNameTem...b_).
  if (OutlinedName.size() == CurrentName.size() &&
      CurrentName.startswith(
          StringRef(OutlinedName.c_str(), OutlinedName.size() - 2)))
    return false;

  return !BridgedArguments.empty();
}

CanSILFunctionType ObjCMethodCall::getOutlinedFunctionType(SILModule &M) {
  auto FunTy = BridgedCall->getSubstCalleeType();
  SmallVector<SILParameterInfo, 4> Parameters;
  unsigned OrigSigIdx = 0;
  unsigned BridgedArgIdx = 0;
  for (auto &ParamInfo : FunTy->getParameters()) {
    // Either use the bridged type passing it @owned.
    if (BridgedArgIdx < BridgedArguments.size() &&
        BridgedArguments[BridgedArgIdx].Idx == OrigSigIdx) {
      auto convention = BridgedArguments[BridgedArgIdx].getConvention();
      Parameters.push_back(SILParameterInfo(BridgedArguments[BridgedArgIdx]
                                                .bridgedValue()
                                                ->getType()
                                                .getASTType(),
                                            convention));
      ++BridgedArgIdx;
    } else {
      // Otherwise, use the original type convention.
      Parameters.push_back(ParamInfo);
    }
    ++OrigSigIdx;
  }

  auto ExtInfo = SILFunctionType::ExtInfo::getThin();

  SmallVector<SILResultInfo, 4> Results;
  // If we don't have a bridged return we changed from @autoreleased to @owned
  // if there is a result.
  if (!BridgedReturn) {
    if (FunTy->getNumResults()) {
      auto OrigResultInfo = FunTy->getSingleResult();
      Results.push_back(SILResultInfo(OrigResultInfo.getInterfaceType(),
                                      OrigResultInfo.getConvention() ==
                                              ResultConvention::Autoreleased
                                          ? ResultConvention::Owned
                                          : OrigResultInfo.getConvention()));
    }
  } else {
    // Otherwise, we used the bridged return type.
    Results.push_back(
        SILResultInfo(BridgedReturn.getReturnType(), ResultConvention::Owned));
  }
  auto FunctionType = SILFunctionType::get(
      nullptr, ExtInfo, SILCoroutineKind::None,
      ParameterConvention::Direct_Unowned, Parameters, {}, Results, llvm::None,
      SubstitutionMap(), SubstitutionMap(), M.getASTContext());
  return FunctionType;
}

namespace {
/// A collection of outlineable patterns.
class OutlinePatterns {
  BridgedProperty BridgedPropertyPattern;
  ObjCMethodCall ObjCMethodCallPattern;

public:
  /// Try matching an outlineable pattern from the current instruction.
  OutlinePattern *tryToMatch(SILBasicBlock::iterator CurInst) {
    if (BridgedPropertyPattern.matchInstSequence(CurInst))
      return &BridgedPropertyPattern;

    if (ObjCMethodCallPattern.matchInstSequence(CurInst))
      return &ObjCMethodCallPattern;

    return nullptr;
  }

  OutlinePatterns(SILOptFunctionBuilder &FuncBuilder,
                  InstModCallbacks callbacks,
                  DeadEndBlocks *deBlocks)
      : BridgedPropertyPattern(FuncBuilder, callbacks, deBlocks),
        ObjCMethodCallPattern(FuncBuilder, callbacks, deBlocks) {}
  ~OutlinePatterns() {}

  OutlinePatterns(const OutlinePatterns&) = delete;
  OutlinePatterns& operator=(const OutlinePatterns) = delete;
};
} // end anonymous namespace.


/// Perform outlining on the function and return any newly created outlined
/// functions.
bool tryOutline(SILOptFunctionBuilder &FuncBuilder, SILFunction *Fun,
                SmallVectorImpl<SILFunction *> &FunctionsAdded,
                InstModCallbacks callbacks = InstModCallbacks(),
                DeadEndBlocks *deBlocks = nullptr) {
  BasicBlockWorklist Worklist(Fun->getEntryBlock());
  OutlinePatterns patterns(FuncBuilder, callbacks, deBlocks);
  bool changed = false;

  // Traverse the function.
  while (SILBasicBlock *CurBlock = Worklist.pop()) {
    SILBasicBlock::iterator CurInst = CurBlock->begin();

    // Go over the instructions trying to match and replace patterns.
    while (CurInst != CurBlock->end()) {
       if (OutlinePattern *match = patterns.tryToMatch(CurInst)) {
         SILFunction *F;
         SILBasicBlock::iterator LastInst;
         std::tie(F, LastInst) = match->outline(Fun->getModule());
         if (F)
           FunctionsAdded.push_back(F);
         CurInst = LastInst;
         assert(LastInst->getParent() == CurBlock);
         changed = true;
       } else if (isa<TermInst>(CurInst)) {
         for (SILBasicBlock *succ : CurBlock->getSuccessors()) {
           Worklist.pushIfNotVisited(succ);
         }
         ++CurInst;
       } else {
         ++CurInst;
       }
    }
  }
  return changed;
}

namespace {

class Outliner : public SILFunctionTransform {
public:
  Outliner() { }

  void run() override {
    auto *Fun = getFunction();

    // Only outline if we optimize for size.
    if (!Fun->optimizeForSize())
      return;

    // Dump function if requested.
    if (DumpFuncsBeforeOutliner.size() &&
        Fun->getName().contains(DumpFuncsBeforeOutliner)) {
      Fun->dump();
    }

    DeadEndBlocksAnalysis *deBlocksAnalysis =
        PM->getAnalysis<DeadEndBlocksAnalysis>();
    DeadEndBlocks *deBlocks = deBlocksAnalysis->get(Fun);
    InstModCallbacks callbacks;

    SILOptFunctionBuilder FuncBuilder(*this);
    SmallVector<SILFunction *, 16> FunctionsAdded;
    bool Changed = false;

    if (Fun->hasOwnership()) {
      Changed =
          tryOutline(FuncBuilder, Fun, FunctionsAdded, callbacks, deBlocks);
    } else {
      Changed = tryOutline(FuncBuilder, Fun, FunctionsAdded);
    }

    if (!FunctionsAdded.empty()) {
      // Notify the pass manager of any new functions we outlined.
      for (auto *AddedFunc : FunctionsAdded) {
        addFunctionToPassManagerWorklist(AddedFunc, nullptr);
      }
    }

    if (Changed) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
    }
  }
};

} //end anonymous namespace.

SILTransform *swift::createOutliner() {
  return new Outliner();
}
