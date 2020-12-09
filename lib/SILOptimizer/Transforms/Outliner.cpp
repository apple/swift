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
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
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
  OutlinerMangler(SILDeclRef Method, bool IsAddress)
      : IsParameterBridged(nullptr), IsParameterGuaranteed(nullptr),
        MethodDecl(Method),
        Kind(IsAddress ? BridgedPropertyAddress : BridgedProperty),
        IsReturnBridged(true) {}

  std::string mangle();

private:
  char getMethodKindMangling() {
    switch (Kind) {
    case BridgedProperty:
      return 'p';
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

public:
  OutlinePattern(SILOptFunctionBuilder &FuncBuilder) : FuncBuilder(FuncBuilder) {}

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

  /// If we have a consuming switch, this is the release_value along the some
  /// branch that destroys the apply.
  ReleaseValueInst *Release = nullptr;
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
  StrongReleaseInst *Release;
  ApplyInst *PropApply;

public:
  bool matchInstSequence(SILBasicBlock::iterator I) override;

  std::pair<SILFunction *, SILBasicBlock::iterator>
  outline(SILModule &M) override;

  BridgedProperty(SILOptFunctionBuilder &FuncBuilder) : OutlinePattern(FuncBuilder) {
    clearState();
  }

  BridgedProperty(const BridgedProperty&) = delete;
  BridgedProperty& operator=(const BridgedProperty&) = delete;

  virtual ~BridgedProperty() {}

  std::string getOutlinedFunctionName() override;

private:
  bool matchMethodCall(SILBasicBlock::iterator);
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
    OutlinedName.clear();
}

std::string BridgedProperty::getOutlinedFunctionName() {
  if (OutlinedName.empty()) {
    OutlinerMangler Mangler(ObjCMethod->getMember(), isa<LoadInst>(FirstInst));
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
    Parameters.push_back(SILParameterInfo(cast<ObjCMethodInst>(FirstInst)
                                              ->getOperand()
                                              ->getType()
                                              .getASTType(),
                                          ParameterConvention::Direct_Unowned));
  SmallVector<SILResultInfo, 4> Results;

  Results.push_back(SILResultInfo(
                      switchInfo.Br->getArg(0)->getType().getASTType(),
                      ResultConvention::Owned));
  auto ExtInfo = SILFunctionType::ExtInfo::getThin();
  auto FunctionType = SILFunctionType::get(
      nullptr, ExtInfo, SILCoroutineKind::None,
      ParameterConvention::Direct_Unowned, Parameters, /*yields*/ {},
      Results, None,
      SubstitutionMap(), SubstitutionMap(),
      M.getASTContext());
  return FunctionType;
}

std::pair<SILFunction *, SILBasicBlock::iterator>
BridgedProperty::outline(SILModule &M) {
  // Get the function type.
  auto FunctionType = getOutlinedFunctionType(M);

  std::string nameTmp = getOutlinedFunctionName();
  auto name = M.allocateCopy(nameTmp);

  auto *Fun = FuncBuilder.getOrCreateFunction(
      ObjCMethod->getLoc(), name, SILLinkage::Shared, FunctionType, IsNotBare,
      IsNotTransparent, IsSerializable, IsNotDynamic);
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
    OutlinedEntryBB->eraseInstructions();
    OutlinedEntryBB->eraseFromParent();
    switchInfo.NoneBB->eraseInstructions();
    switchInfo.NoneBB->eraseFromParent();
    switchInfo.SomeBB->eraseInstructions();
    switchInfo.SomeBB->eraseFromParent();
    OldMergeBB->eraseInstructions();
    OldMergeBB->eraseFromParent();
    return std::make_pair(nullptr, std::prev(StartBB->end()));
  }

  if (!OutlinedEntryBB->getParent()->hasOwnership())
    Fun->setOwnershipEliminated();

  Fun->setInlineStrategy(NoInline);

  // Move the blocks into the new function.
  auto &FromBlockList = OutlinedEntryBB->getParent()->getBlocks();
  Fun->getBlocks().splice(Fun->begin(), FromBlockList, OldMergeBB);
  Fun->getBlocks().splice(Fun->begin(), FromBlockList, switchInfo.NoneBB);
  Fun->getBlocks().splice(Fun->begin(), FromBlockList, switchInfo.SomeBB);
  Fun->getBlocks().splice(Fun->begin(), FromBlockList, OutlinedEntryBB);

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

static bool matchSwitch(SwitchInfo &SI, SwitchEnumInst *SwitchEnum,
                        SILValue SwitchOperand) {
  if (SwitchEnum->getNumCases() != 2 ||
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
  auto *BridgeFun = FunRef->getInitiallyReferencedFunction();
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
    // release_value %38 : $Optional<NSString>
    ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
    auto *RVI = dyn_cast<ReleaseValueInst>(It);
    if (!RVI || RVI->getOperand() != SomeEnum)
      return false;
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

/// Match a switch where our operand's lifetime ends inside the switch.
static bool matchConsumingSwitch(SwitchInfo &SI, SwitchEnumInst *SwitchEnum,
                                 SILValue SwitchOperand) {
  if (SwitchEnum->getNumCases() != 2 ||
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

  // We pattern match a bb with a single br in it.
  auto It = NoneBB->begin();
  auto *Br1 = dyn_cast<BranchInst>(It);
  if (!Br1 || Br1->getNumArgs() != 0)
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
  if (!SomeEnum || !SomeEnum->hasOperand() ||
      SomeEnum->getOperand() != SomeBBArg)
    return false;
  size_t numSomeEnumUses =
      std::distance(SomeEnum->use_begin(), SomeEnum->use_end());
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
  auto *BridgeFun = FunRef->getInitiallyReferencedFunction();
  auto *SwiftModule = BridgeFun->getModule().getSwiftModule();
  // Not every type conforms to the ObjectiveCBridgeable protocol in such a case
  // getBridgeFromObjectiveC returns SILDeclRef().
  auto bridgeWitness = getBridgeFromObjectiveC(NativeType, SwiftModule);
  if (bridgeWitness == SILDeclRef() ||
      BridgeFun->getName() != bridgeWitness.mangle())
    return false;

  // release_value %41 : $Optional<String>
  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  auto *rvi = dyn_cast<ReleaseValueInst>(It);
  if (!rvi || rvi->getOperand() != Apply)
    return false;

  if (numSomeEnumUses == 2) {
    // release_value %38 : $Optional<NSString>
    ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
    auto *someEnumRVI = dyn_cast<ReleaseValueInst>(It);
    if (!someEnumRVI || someEnumRVI->getOperand() != SomeEnum)
      return false;
  }

  // br bb10
  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  auto *br = dyn_cast<BranchInst>(It);
  if (!br || br->getDestBB() != MergeBB || br->getNumArgs() != 0)
    return false;

  SI.SwitchEnum = SwitchEnum;
  SI.SomeBB = SomeBB;
  SI.NoneBB = NoneBB;
  SI.Br = br;
  SI.Release = rvi;

  return true;
}

bool BridgedProperty::matchMethodCall(SILBasicBlock::iterator It) {
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

  // Don't outline in the outlined function.
  if (ObjCMethod->getFunction()->getName().equals(getOutlinedFunctionName()))
    return false;

  // %34 = apply %33(%31) : $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  PropApply = dyn_cast<ApplyInst>(It);
  if (!PropApply || PropApply->getCallee() != ObjCMethod ||
      PropApply->getNumArguments() != 1 ||
      PropApply->getArgument(0) != Instance || !PropApply->hasOneUse())
    return false;

  // switch_enum %34 : $Optional<NSString>, case #Optional.some!enumelt: bb8, case #Optional.none!enumelt: bb9
  ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  auto *switchEnum = dyn_cast<SwitchEnumInst>(&*It);
  if (!switchEnum)
    return false;

  if (matchSwitch(switchInfo, switchEnum, PropApply))
    return true;

  return matchConsumingSwitch(switchInfo, switchEnum, PropApply);
}

bool BridgedProperty::matchInstSequence(SILBasicBlock::iterator It) {
  // Matches:
  // [ optionally:
  //    %31 = load %30 : $*UITextField
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
    // strong_retain %31 : $UITextField
    ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
    auto *Retain = dyn_cast<StrongRetainInst>(It);
    if (!Retain || Retain->getOperand() != Load)
      return false;
    ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
  }

  if (!matchMethodCall(It))
    return false;

  if (Load) {
    // There will be a release matching the earlier retain. The only user of the
    // retained value is the unowned objective-c method consumer.
    unsigned NumUses = 0;
    Release = nullptr;
    for (auto *Use : Load->getUses()) {
      ++NumUses;
      if (auto *R = dyn_cast<StrongReleaseInst>(Use->getUser())) {
        if (!Release) {
          Release = R;
        } else {
          Release = nullptr;
          break;
        }
      }
    }
    if (!Release || NumUses != 4)
      return false;
  }
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
  ReleaseValueInst *ReleaseAfterBridge;
  ReleaseValueInst *ReleaseArgAfterCall;
  unsigned Idx = 0;

  // Matched bridged argument.
  BridgedArgument(unsigned Idx, FunctionRefInst *F, ApplyInst *A, EnumInst *E,
                  ReleaseValueInst *R0, ReleaseValueInst *R1)
      : BridgeFun(F), BridgeCall(A), OptionalResult(E),
        BridgedValue(FullApplySite(A).getSelfArgument()),
        ReleaseAfterBridge(R0), ReleaseArgAfterCall(R1), Idx(Idx) {}

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
    ReleaseAfterBridge->setOperand(BridgedValue);
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

static ReleaseValueInst *findReleaseOf(SILValue releasedValue,
                                       SILBasicBlock::iterator from,
                                       SILBasicBlock::iterator to) {
  while (from != to) {
    auto release = dyn_cast<ReleaseValueInst>(&*from);
    if (release && release->getOperand() == releasedValue)
      return release;
    ++from;
  }
  return nullptr;
}

BridgedArgument BridgedArgument::match(unsigned ArgIdx, SILValue Arg,
                                       ApplyInst *AI) {
  // Match
  // %15 = function_ref @$SSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF
  // %16 = apply %15(%14) :
  //         $@convention(method) (@guaranteed String) -> @owned NSString
  // %17 = enum $Optional<NSString>, #Optional.some!enumelt, %16 : $NSString
  // release_value %14 : $String
  // ...
  // apply %objcMethod(%17, ...) : $@convention(objc_method) (Optional<NSString> ...) ->
  // release_value ...
  // release_value %17 : $Optional<NSString>
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
  auto *BridgedValueRelease = dyn_cast_or_null<ReleaseValueInst>(
      findReleaseOf(BridgedValue, std::next(SILBasicBlock::iterator(Enum)),
                    SILBasicBlock::iterator(AI)));
  if (BridgedValueRelease && BridgedValueRelease->getOperand() != BridgedValue)
    return BridgedArgument();

  if (SILBasicBlock::iterator(BridgeCall) == BridgeCall->getParent()->begin())
    return BridgedArgument();
  auto *FunRef =
      dyn_cast<FunctionRefInst>(std::prev(SILBasicBlock::iterator(BridgeCall)));
  if (!FunRef || !FunRef->hasOneUse() || BridgeCall->getCallee() != FunRef)
    return BridgedArgument();

  ReleaseValueInst *ReleaseAfter = nullptr;
  for (auto *Use : Enum->getUses()) {
    if (Use->getUser() == AI)
      continue;

    // The enum must only have two uses the release and the apply.
    if (ReleaseAfter)
      return BridgedArgument();

    ReleaseAfter = dyn_cast<ReleaseValueInst>(Use->getUser());
    if (!ReleaseAfter)
      return BridgedArgument();
  }

  // Make sure we are calling the actual bridge witness.
  auto NativeType = BridgedValue->getType().getASTType();
  auto *BridgeFun = FunRef->getInitiallyReferencedFunction();
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

// Match one of the following return value briding patterns.
//
// Non-Consuming Pattern:
//
// ```
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
// ```
//
// Consuming Pattern:
//
// ```
//   switch_enum %20 : $Optional<NSString>, case #O.some: bb1, case #O.none: bb2
//
// bb1(%23 : $NSString):
//   %24 = function_ref @_unconditionallyBridgeFromObjectiveC
//   %25 = enum $Optional<NSString>, #Optional.some!enumelt, %23 : $NSString
//   %26 = metatype $@thin String.Type
//   %27 = apply %24(%25, %26)
//   %28 = enum $Optional<String>, #Optional.some!enumelt, %27 : $String
//   release_value %28 : $Optional<String>
//   release_value %25 : $Optional<String>
//   br bb3
//
// bb2:
//   br bb3
//
// bb3:
//   ...
class BridgedReturn {
  SwitchInfo switchInfo;
public:
  bool match(ApplyInst *BridgedCall) {
    switchInfo = SwitchInfo();
    auto *SwitchBB = BridgedCall->getParent();
    auto *SwitchEnum = dyn_cast<SwitchEnumInst>(SwitchBB->getTerminator());
    if (!SwitchEnum)
      return false;
    if (matchSwitch(switchInfo, SwitchEnum, BridgedCall))
      return true;
    return matchConsumingSwitch(switchInfo, SwitchEnum, BridgedCall);
  }

  operator bool() { return switchInfo.SomeBB != nullptr; }

  CanType getReturnType() {
    // If switchInfo.Release is set, then we know that we had a consuming return
    // and thus we end the life of the value before the branch (and thus our
    // branch has no arg). Even though this is consuming, we are going to fix up
    // the function so we can use the same outlined function in all cases.
    if (auto *rvi = switchInfo.Release) {
      auto type = rvi->getOperand()->getType();
      return SILType::getOptionalType(type).getASTType();
    }

    return switchInfo.Br->getArg(0)->getType().getASTType();
  }

  /// Outline the return value bridging blocks.
  void outline(SILFunction *Fun, ApplyInst *NewOutlinedCall);
};
}

void BridgedReturn::outline(SILFunction *Fun, ApplyInst *NewOutlinedCall) {
  // Outline the bridged return result blocks handling both of the following
  // patterns:
  //
  // Non Consuming Pattern
  //
  // ```
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
  // ```
  //
  // Consuming Pattern
  //
  // ```
  //   switch_enum %20 : $Optional<NSString>, case #O.some: bb1, case #O.none: bb2
  //
  // bb1(%23 : $NSString):
  //   %24 = function_ref @$SSS10FoundationE36_unconditionallyBridgeFromObjectiveC
  //   %25 = enum $Optional<NSString>, #Optional.some!enumelt, %23 : $NSString
  //   %26 = metatype $@thin String.Type
  //   %27 = apply %24(%25, %26)
  //   release_value %27 : $Optional<String>
  //   release_value %25 : $Optional<NSString>
  //   br bb3
  //
  // bb2:
  //   br bb3
  //
  // bb3:
  // ```
  //
  // In the consuming case, we assume that we are going to insert a fake return
  // and assert in our caller.
  auto *StartBB = switchInfo.SwitchEnum->getParent();
  auto *OutlinedEntryBB = StartBB->split(SILBasicBlock::iterator(switchInfo.SwitchEnum));
  auto *OldMergeBB = switchInfo.Br->getDestBB();
  auto *NewTailBB = OldMergeBB->split(OldMergeBB->begin());
  auto Loc = switchInfo.SwitchEnum->getLoc();

  {
    SILBuilder Builder(StartBB);
    Builder.createBranch(Loc, NewTailBB);
    if (!switchInfo.Release) {
      // If we aren't matching a consuming pattern, RAUW old merge bb with our
      // new outlined call.
      OldMergeBB->getArgument(0)->replaceAllUsesWith(NewOutlinedCall);
    }
  }

  // If we have a release, move it into the new tail BB and set it to be
  // NewOutlinedCall.
  SILValue oldReleaseValue;
  if (auto *release = switchInfo.Release) {
    release->moveFront(NewTailBB);
    auto &op = release->getAllOperands()[0];
    oldReleaseValue = op.get();
    op.set(NewOutlinedCall);
  }

  // Outlined function already existed. Just delete instructions and wire up
  // blocks.
  if (!Fun) {
    OutlinedEntryBB->eraseInstructions();
    OutlinedEntryBB->eraseFromParent();
    switchInfo.NoneBB->eraseInstructions();
    switchInfo.NoneBB->eraseFromParent();
    switchInfo.SomeBB->eraseInstructions();
    switchInfo.SomeBB->eraseFromParent();
    OldMergeBB->eraseInstructions();
    OldMergeBB->eraseFromParent();
    return;
  }

  // See if we have had a release. In such a case, we need to do some fixups to
  // convert our consuming case to the standard non-consuming case by inserting
  // branches, forwarding enums, etc. We already moved the release into
  // NewTailBB, so we will release our value appropriately when we return it
  // from NewOutlinedCall.
  if (switchInfo.Release) {
    auto loc = RegularLocation::getAutoGeneratedLocation();
    auto resultType = NewOutlinedCall->getType();
    OldMergeBB->createPhiArgument(resultType, OwnershipKind::None);
    {
      SILBuilder Builder(switchInfo.NoneBB->getTerminator());
      auto *none = Builder.createOptionalNone(loc, resultType);
      addNewEdgeValueToBranch(switchInfo.NoneBB->getTerminator(), OldMergeBB,
                              none);
    }

    {
      SILBuilder Builder(switchInfo.SomeBB->getTerminator());
      assert(oldReleaseValue);
      auto *some = Builder.createOptionalSome(loc, oldReleaseValue, resultType);
      addNewEdgeValueToBranch(switchInfo.SomeBB->getTerminator(), OldMergeBB,
                              some);
    }
  }

  // Move the blocks into the new function.
  assert(Fun->begin() != Fun->end() &&
         "The entry block must already have been created");
  SILBasicBlock *EntryBB = &*Fun->begin();
  auto &FromBlockList = OutlinedEntryBB->getParent()->getBlocks();
  Fun->getBlocks().splice(Fun->begin(), FromBlockList, OldMergeBB);
  OldMergeBB->moveAfter(EntryBB);

  auto InsertPt = SILFunction::iterator(OldMergeBB);
  Fun->getBlocks().splice(InsertPt, FromBlockList, OutlinedEntryBB);
  Fun->getBlocks().splice(InsertPt, FromBlockList, switchInfo.NoneBB);
  Fun->getBlocks().splice(InsertPt, FromBlockList, switchInfo.SomeBB);

  SILBuilder Builder(EntryBB);
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

  ObjCMethodCall(SILOptFunctionBuilder &FuncBuilder)
      : OutlinePattern(FuncBuilder) {}
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
      IsNotTransparent, IsSerializable, IsNotDynamic);
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
        Args.push_back(BridgedArguments[BridgedArgIdx].bridgedValue());
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
      ParameterConvention::Direct_Unowned, Parameters, {},
      Results, None,
      SubstitutionMap(), SubstitutionMap(),
      M.getASTContext());
  return FunctionType;
}

namespace {
/// A collection of outlineable patterns.
class OutlinePatterns {
  BridgedProperty BridgedPropertyPattern;
  ObjCMethodCall ObjCMethodCallPattern;
  llvm::DenseMap<CanType, SILDeclRef> BridgeToObjectiveCCache;
  llvm::DenseMap<CanType, SILDeclRef> BridgeFromObjectiveCache;

public:
  /// Try matching an outlineable pattern from the current instruction.
  OutlinePattern *tryToMatch(SILBasicBlock::iterator CurInst) {
    if (BridgedPropertyPattern.matchInstSequence(CurInst))
      return &BridgedPropertyPattern;

    if (ObjCMethodCallPattern.matchInstSequence(CurInst))
      return &ObjCMethodCallPattern;

    return nullptr;
  }

  OutlinePatterns(SILOptFunctionBuilder &FuncBuilder)
      : BridgedPropertyPattern(FuncBuilder),
        ObjCMethodCallPattern(FuncBuilder) {}
  ~OutlinePatterns() {}

  OutlinePatterns(const OutlinePatterns&) = delete;
  OutlinePatterns& operator=(const OutlinePatterns) = delete;
};
} // end anonymous namespace.


/// Perform outlining on the function and return any newly created outlined
/// functions.
bool tryOutline(SILOptFunctionBuilder &FuncBuilder, SILFunction *Fun,
                SmallVectorImpl<SILFunction *> &FunctionsAdded) {
  SmallPtrSet<SILBasicBlock *, 32> Visited;
  SmallVector<SILBasicBlock *, 128> Worklist;
  OutlinePatterns patterns(FuncBuilder);
  bool changed = false;

  // Traverse the function.
  Worklist.push_back(&*Fun->begin());
  while (!Worklist.empty()) {

    SILBasicBlock *CurBlock = Worklist.pop_back_val();
    if (!Visited.insert(CurBlock).second) continue;

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
         std::copy(CurBlock->succ_begin(), CurBlock->succ_end(),
                   std::back_inserter(Worklist));
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

    // We do not support [ossa] now.
    if (Fun->hasOwnership())
      return;

    // Only outline if we optimize for size.
    if (!Fun->optimizeForSize())
      return;

    // Dump function if requested.
    if (DumpFuncsBeforeOutliner.size() &&
        Fun->getName().find(DumpFuncsBeforeOutliner, 0) != StringRef::npos) {
      Fun->dump();
    }

    SILOptFunctionBuilder FuncBuilder(*this);
    SmallVector<SILFunction *, 16> FunctionsAdded;
    bool Changed = tryOutline(FuncBuilder, Fun, FunctionsAdded);

    if (!FunctionsAdded.empty()) {
      // Notify the pass manager of any new functions we outlined.
      for (auto *AddedFunc : FunctionsAdded) {
        addFunctionToPassManagerWorklist(AddedFunc, nullptr);
      }
    }

    if (Changed) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Everything);
    }
  }
};

} //end anonymous namespace.

SILTransform *swift::createOutliner() {
  return new Outliner();
}
