//===--- SILFunction.cpp - Defines the SILFunction data structure ---------===//
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

#define DEBUG_TYPE "sil-function"

#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBridging.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILProfiler.h"
#include "swift/SIL/CFG.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/AST/Availability.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/Stmt.h"
#include "swift/Basic/OptimizationMode.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/BridgingUtils.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/GraphWriter.h"
#include "clang/AST/Decl.h"

using namespace swift;
using namespace Lowering;

SILSpecializeAttr::SILSpecializeAttr(bool exported, SpecializationKind kind,
                                     GenericSignature specializedSig,
                                     GenericSignature unerasedSpecializedSig,
                                     ArrayRef<Type> typeErasedParams,
                                     SILFunction *target, Identifier spiGroup,
                                     const ModuleDecl *spiModule,
                                     AvailabilityContext availability)
    : kind(kind), exported(exported), specializedSignature(specializedSig),
      unerasedSpecializedSignature(unerasedSpecializedSig),
      typeErasedParams(typeErasedParams.begin(), typeErasedParams.end()),
      spiGroup(spiGroup), availability(availability), spiModule(spiModule),
      targetFunction(target) {
  if (targetFunction)
    targetFunction->incrementRefCount();
}

SILSpecializeAttr *
SILSpecializeAttr::create(SILModule &M, GenericSignature specializedSig,
                          ArrayRef<Type> typeErasedParams,
                          bool exported, SpecializationKind kind,
                          SILFunction *target, Identifier spiGroup,
                          const ModuleDecl *spiModule,
                          AvailabilityContext availability) {
  auto erasedSpecializedSig = specializedSig.typeErased(typeErasedParams);

  void *buf = M.allocate(sizeof(SILSpecializeAttr), alignof(SILSpecializeAttr));

  return ::new (buf) SILSpecializeAttr(exported, kind, erasedSpecializedSig,
                                       specializedSig, typeErasedParams, target,
                                       spiGroup, spiModule, availability);
}

void SILFunction::addSpecializeAttr(SILSpecializeAttr *Attr) {
  if (getLoweredFunctionType()->getInvocationGenericSignature()) {
    Attr->F = this;
    SpecializeAttrSet.push_back(Attr);
  }
}

void SILFunction::removeSpecializeAttr(SILSpecializeAttr *attr) {
  // Drop the reference to the _specialize(target:) function.
  if (auto *targetFun = attr->getTargetFunction()) {
    targetFun->decrementRefCount();
  }
  SpecializeAttrSet.erase(std::remove_if(SpecializeAttrSet.begin(),
                                         SpecializeAttrSet.end(),
                                         [attr](SILSpecializeAttr *member) {
                                           return member == attr;
                                         }),
                          SpecializeAttrSet.end());
}

SILFunction *
SILFunction::create(SILModule &M, SILLinkage linkage, StringRef name,
                    CanSILFunctionType loweredType,
                    GenericEnvironment *genericEnv, Optional<SILLocation> loc,
                    IsBare_t isBareSILFunction, IsTransparent_t isTrans,
                    IsSerialized_t isSerialized, ProfileCounter entryCount,
                    IsDynamicallyReplaceable_t isDynamic,
                    IsDistributed_t isDistributed,
                    IsRuntimeAccessible_t isRuntimeAccessible,
                    IsExactSelfClass_t isExactSelfClass,
                    IsThunk_t isThunk,
                    SubclassScope classSubclassScope, Inline_t inlineStrategy,
                    EffectsKind E, SILFunction *insertBefore,
                    const SILDebugScope *debugScope) {
  // Get a StringMapEntry for the function.  As a sop to error cases,
  // allow the name to have an empty string.
  llvm::StringMapEntry<SILFunction*> *entry = nullptr;
  if (!name.empty()) {
    entry = &*M.FunctionTable.insert(std::make_pair(name, nullptr)).first;
    PrettyStackTraceSILFunction trace("creating", entry->getValue());
    assert(!entry->getValue() && "function already exists");
    name = entry->getKey();
  }

  SILFunction *fn = M.removeFromZombieList(name);
  if (fn) {
    // Resurrect a zombie function.
    // This happens for example if a specialized function gets dead and gets
    // deleted. And afterwards the same specialization is created again.
    fn->init(linkage, name, loweredType, genericEnv, isBareSILFunction, isTrans,
             isSerialized, entryCount, isThunk, classSubclassScope,
             inlineStrategy, E, debugScope, isDynamic, isExactSelfClass,
             isDistributed, isRuntimeAccessible);
    assert(fn->empty());
  } else {
    fn = new (M) SILFunction(
        M, linkage, name, loweredType, genericEnv, isBareSILFunction, isTrans,
        isSerialized, entryCount, isThunk, classSubclassScope, inlineStrategy,
        E, debugScope, isDynamic, isExactSelfClass, isDistributed,
        isRuntimeAccessible);
  }
  if (entry) entry->setValue(fn);

  if (insertBefore)
    M.functions.insert(SILModule::iterator(insertBefore), fn);
  else
    M.functions.push_back(fn);

  return fn;
}

static SwiftMetatype functionMetatype;
static BridgedFunction::RegisterFn initFunction = nullptr;
static BridgedFunction::RegisterFn destroyFunction = nullptr;
static BridgedFunction::WriteFn writeFunction = nullptr;
static BridgedFunction::ParseFn parseFunction = nullptr;
static BridgedFunction::CopyEffectsFn copyEffectsFunction = nullptr;
static BridgedFunction::GetEffectInfoFn getEffectInfoFunction = nullptr;
static BridgedFunction::GetMemBehaviorFn getMemBehvaiorFunction = nullptr;

SILFunction::SILFunction(
    SILModule &Module, SILLinkage Linkage, StringRef Name,
    CanSILFunctionType LoweredType, GenericEnvironment *genericEnv,
    IsBare_t isBareSILFunction, IsTransparent_t isTrans,
    IsSerialized_t isSerialized, ProfileCounter entryCount, IsThunk_t isThunk,
    SubclassScope classSubclassScope, Inline_t inlineStrategy, EffectsKind E,
    const SILDebugScope *DebugScope, IsDynamicallyReplaceable_t isDynamic,
    IsExactSelfClass_t isExactSelfClass, IsDistributed_t isDistributed,
    IsRuntimeAccessible_t isRuntimeAccessible)
    : SwiftObjectHeader(functionMetatype), Module(Module),
      index(Module.getNewFunctionIndex()),
      Availability(AvailabilityContext::alwaysAvailable()) {
  init(Linkage, Name, LoweredType, genericEnv, isBareSILFunction, isTrans,
       isSerialized, entryCount, isThunk, classSubclassScope, inlineStrategy, E,
       DebugScope, isDynamic, isExactSelfClass, isDistributed,
       isRuntimeAccessible);

  // Set our BB list to have this function as its parent. This enables us to
  // splice efficiently basic blocks in between functions.
  BlockList.Parent = this;
  if (initFunction)
    initFunction({this}, &libswiftSpecificData, sizeof(libswiftSpecificData));
}

void SILFunction::init(
    SILLinkage Linkage, StringRef Name, CanSILFunctionType LoweredType,
    GenericEnvironment *genericEnv, IsBare_t isBareSILFunction,
    IsTransparent_t isTrans, IsSerialized_t isSerialized,
    ProfileCounter entryCount, IsThunk_t isThunk,
    SubclassScope classSubclassScope, Inline_t inlineStrategy, EffectsKind E,
    const SILDebugScope *DebugScope, IsDynamicallyReplaceable_t isDynamic,
    IsExactSelfClass_t isExactSelfClass, IsDistributed_t isDistributed,
    IsRuntimeAccessible_t isRuntimeAccessible) {
  setName(Name);

  assert(!LoweredType->hasTypeParameter() &&
         "function type has open type parameters");

  this->LoweredType = LoweredType;
  this->GenericEnv = genericEnv;
  this->SpecializationInfo = nullptr;
  this->EntryCount = entryCount;
  this->Availability = AvailabilityContext::alwaysAvailable();
  this->Bare = isBareSILFunction;
  this->Transparent = isTrans;
  this->Serialized = isSerialized;
  this->Thunk = isThunk;
  this->ClassSubclassScope = unsigned(classSubclassScope);
  this->GlobalInitFlag = false;
  this->InlineStrategy = inlineStrategy;
  this->Linkage = unsigned(Linkage);
  this->HasCReferences = false;
  this->MarkedAsUsed = false;
  this->IsAlwaysWeakImported = false;
  this->IsDynamicReplaceable = isDynamic;
  this->ExactSelfClass = isExactSelfClass;
  this->IsDistributed = isDistributed;
  this->IsRuntimeAccessible = isRuntimeAccessible;
  this->ForceEnableLexicalLifetimes = DoNotForceEnableLexicalLifetimes;
  this->UseStackForPackMetadata = DoUseStackForPackMetadata;
  this->stackProtection = false;
  this->Inlined = false;
  this->Zombie = false;
  this->HasOwnership = true,
  this->WasDeserializedCanonical = false;
  this->IsStaticallyLinked = false;
  this->IsWithoutActuallyEscapingThunk = false;
  this->OptMode = unsigned(OptimizationMode::NotSet);
  this->perfConstraints = PerformanceConstraints::None;
  this->EffectsKindAttr = unsigned(E);
  assert(!Transparent || !IsDynamicReplaceable);
  validateSubclassScope(classSubclassScope, isThunk, nullptr);
  setDebugScope(DebugScope);
}

SILFunction::~SILFunction() {
  // If the function is recursive, a function_ref inst inside of the function
  // will give the function a non-zero ref count triggering the assertion. Thus
  // we drop all instruction references before we erase.
  // We also need to drop all references if instructions are allocated using
  // an allocator that may recycle freed memory.
  dropAllReferences();
  if (snapshots)
    snapshots->~SILFunction();

  if (ReplacedFunction) {
    ReplacedFunction->decrementRefCount();
    ReplacedFunction = nullptr;
  }

  auto &M = getModule();
  for (auto &BB : *this) {
    BB.eraseAllInstructions(M);
  }

  assert(RefCount == 0 &&
         "Function cannot be deleted while function_ref's still exist");
  assert(!newestAliveBlockBitfield &&
         "Not all BasicBlockBitfields deleted at function destruction");
  assert(!newestAliveNodeBitfield &&
         "Not all NodeBitfields deleted at function destruction");

  if (destroyFunction)
    destroyFunction({this}, &libswiftSpecificData, sizeof(libswiftSpecificData));
}

void SILFunction::createSnapshot(int id) {
  assert(id != 0 && "invalid snapshot ID");
  assert(!getSnapshot(id) && "duplicate snapshot");

  SILFunction *newSnapshot = new (Module) SILFunction(
      Module, getLinkage(), getName(), getLoweredFunctionType(),
      getGenericEnvironment(), isBare(), isTransparent(), isSerialized(),
      getEntryCount(), isThunk(), getClassSubclassScope(), getInlineStrategy(),
      getEffectsKind(), getDebugScope(), isDynamicallyReplaceable(),
      isExactSelfClass(), isDistributed(), isRuntimeAccessible());

  // Copy all relevant properties.
  // TODO: It's really unfortunate that this needs to be done manually. It would
  //       be nice if all the properties are encapsulated into a single state,
  //       which can be copied at once.
  newSnapshot->SpecializationInfo = SpecializationInfo;
  newSnapshot->ClangNodeOwner = ClangNodeOwner;
  newSnapshot->DeclCtxt = DeclCtxt;
  newSnapshot->Profiler = Profiler;
  newSnapshot->ReplacedFunction = ReplacedFunction;
  newSnapshot->RefAdHocRequirementFunction = RefAdHocRequirementFunction;
  newSnapshot->ObjCReplacementFor = ObjCReplacementFor;
  newSnapshot->SemanticsAttrSet = SemanticsAttrSet;
  newSnapshot->SpecializeAttrSet = SpecializeAttrSet;
  newSnapshot->Section = Section;
  newSnapshot->Availability = Availability;
  newSnapshot->specialPurpose = specialPurpose;
  newSnapshot->perfConstraints = perfConstraints;
  newSnapshot->GlobalInitFlag = GlobalInitFlag;
  newSnapshot->HasCReferences = HasCReferences;
  newSnapshot->MarkedAsUsed = MarkedAsUsed;
  newSnapshot->IsAlwaysWeakImported = IsAlwaysWeakImported;
  newSnapshot->HasOwnership = HasOwnership;
  newSnapshot->IsWithoutActuallyEscapingThunk = IsWithoutActuallyEscapingThunk;
  newSnapshot->OptMode = OptMode;
  newSnapshot->IsStaticallyLinked = IsStaticallyLinked;
  newSnapshot->copyEffects(this);

  SILFunctionCloner cloner(newSnapshot);
  cloner.cloneFunction(this);

  newSnapshot->snapshotID = id;
  newSnapshot->snapshots = this->snapshots;
  this->snapshots = newSnapshot;

  // The cloner sometimes removes temporary instructions.
  getModule().flushDeletedInsts();
}

SILFunction *SILFunction::getSnapshot(int ID) {
  SILFunction *sn = this;
  do {
    if (sn->snapshotID == ID)
      return sn;
    sn = sn->snapshots;
  } while (sn);
  return nullptr;
}

void SILFunction::restoreFromSnapshot(int ID) {
  SILFunction *sn = getSnapshot(ID);
  assert(sn && "no snapshot found");

  clear();
  SILFunctionCloner cloner(this);
  cloner.cloneFunction(sn);

  // Beside the function body, only restore those properties, which are/can be
  // modified by passes.
  // TODO: There should be a clear sepratation from initialize-once properties
  //       (`let`) and properties which can be modified by passes (`var`).
  copyEffects(sn);

  // The cloner sometimes removes temporary instructions.
  getModule().flushDeletedInsts();
}

void SILFunction::deleteSnapshot(int ID) {
  SILFunction *f = this;
  do {
    if (SILFunction *sn = f->snapshots) {
      if (sn->snapshotID == ID) {
        f->snapshots = sn->snapshots;
        sn->snapshots = nullptr;
        sn->~SILFunction();
        getModule().flushDeletedInsts();
        return;
      }
    }
  } while ((f = f->snapshots) != nullptr);
}

void SILFunction::createProfiler(SILDeclRef Ref) {
  assert(!Profiler && "Function already has a profiler");
  assert(Ref && "Must have non-null SILDeclRef");

  Profiler = SILProfiler::create(Module, Ref);
  if (!Profiler)
    return;

  // If we loaded a profile, set the entry counts for functions and closures
  // for PGO to use.
  if (Ref.isFunc()) {
    if (auto *Closure = Ref.getAbstractClosureExpr()) {
      setEntryCount(Profiler->getExecutionCount(Closure));
    } else {
      auto *FD = Ref.getFuncDecl();
      assert(FD);
      setEntryCount(Profiler->getExecutionCount(FD->getBody()));
    }
  }
}

bool SILFunction::hasForeignBody() const {
  if (!hasClangNode()) return false;
  return SILDeclRef::isClangGenerated(getClangNode());
}

const SILFunction *SILFunction::getOriginOfSpecialization() const {
  if (!isSpecialization())
    return nullptr;

  const SILFunction *p = getSpecializationInfo()->getParent();
  while (p->isSpecialization()) {
    p = p->getSpecializationInfo()->getParent();
  }
  return p;
}

GenericSignature SILFunction::getGenericSignature() const {
  return GenericEnv ? GenericEnv->getGenericSignature() : GenericSignature();
}

void SILFunction::numberValues(llvm::DenseMap<const SILNode*, unsigned> &
                                 ValueToNumberMap) const {
  unsigned idx = 0;
  for (auto &BB : *this) {
    for (auto I = BB.args_begin(), E = BB.args_end(); I != E; ++I)
      ValueToNumberMap[*I] = idx++;
    
    for (auto &I : BB) {
      auto results = I.getResults();
      if (results.empty()) {
        ValueToNumberMap[I.asSILNode()] = idx++;
      } else {
        // Assign the instruction node the first result ID.
        ValueToNumberMap[I.asSILNode()] = idx;
        for (auto result : results) {
          ValueToNumberMap[result] = idx++;
        }
      }
    }
  }
}


ASTContext &SILFunction::getASTContext() const {
  return getModule().getASTContext();
}

OptimizationMode SILFunction::getEffectiveOptimizationMode() const {
  if (OptimizationMode(OptMode) != OptimizationMode::NotSet)
    return OptimizationMode(OptMode);

  return getModule().getOptions().OptMode;
}

bool SILFunction::preserveDebugInfo() const {
  return getEffectiveOptimizationMode() <= OptimizationMode::NoOptimization;
}

bool SILFunction::shouldOptimize() const {
  return getEffectiveOptimizationMode() != OptimizationMode::NoOptimization;
}

Type SILFunction::mapTypeIntoContext(Type type) const {
  return GenericEnvironment::mapTypeIntoContext(
      getGenericEnvironment(), type);
}

SILType SILFunction::mapTypeIntoContext(SILType type) const {
  if (auto *genericEnv = getGenericEnvironment())
    return genericEnv->mapTypeIntoContext(getModule(), type);
  return type;
}

SILType GenericEnvironment::mapTypeIntoContext(SILModule &M,
                                               SILType type) const {
  assert(!type.hasArchetype());

  auto genericSig = getGenericSignature().getCanonicalSignature();
  return type.subst(M,
                    QueryInterfaceTypeSubstitutions(this),
                    LookUpConformanceInSignature(genericSig.getPointer()),
                    genericSig,
                    SubstFlags::PreservePackExpansionLevel);
}

bool SILFunction::isNoReturnFunction(TypeExpansionContext context) const {
  return SILType::getPrimitiveObjectType(getLoweredFunctionType())
      .isNoReturnFunction(getModule(), context);
}

const TypeLowering &
SILFunction::getTypeLowering(AbstractionPattern orig, Type subst) {
  return getModule().Types.getTypeLowering(orig, subst,
                                           TypeExpansionContext(*this));
}

const TypeLowering &SILFunction::getTypeLowering(Type t) const {
  return getModule().Types.getTypeLowering(t, TypeExpansionContext(*this));
}

SILType
SILFunction::getLoweredType(AbstractionPattern orig, Type subst) const {
  return getModule().Types.getLoweredType(orig, subst,
                                          TypeExpansionContext(*this));
}

SILType SILFunction::getLoweredType(Type t) const {
  return getModule().Types.getLoweredType(t, TypeExpansionContext(*this));
}

CanType
SILFunction::getLoweredRValueType(AbstractionPattern orig, Type subst) const {
  return getModule().Types.getLoweredRValueType(TypeExpansionContext(*this),
                                                orig, subst);
}

CanType SILFunction::getLoweredRValueType(Type t) const {
  return getModule().Types.getLoweredRValueType(TypeExpansionContext(*this), t);
}

SILType SILFunction::getLoweredLoadableType(Type t) const {
  auto &M = getModule();
  return M.Types.getLoweredLoadableType(t, TypeExpansionContext(*this), M);
}

const TypeLowering &SILFunction::getTypeLowering(SILType type) const {
  return getModule().Types.getTypeLowering(type, *this);
}

SILType SILFunction::getLoweredType(SILType t) const {
  return getTypeLowering(t).getLoweredType().getCategoryType(t.getCategory());
}
bool SILFunction::isTypeABIAccessible(SILType type) const {
  return getModule().isTypeABIAccessible(type, TypeExpansionContext(*this));
}

bool SILFunction::isWeakImported(ModuleDecl *module) const {
  if (auto *parent = getParentModule())
    if (module->isImportedAsWeakLinked(parent))
      return true;

  // For imported functions check the Clang declaration.
  if (ClangNodeOwner)
    return ClangNodeOwner->getClangDecl()->isWeakImported();

  // For native functions check a flag on the SILFunction
  // itself.
  if (!isAvailableExternally())
    return false;

  if (isAlwaysWeakImported())
    return true;

  if (Availability.isAlwaysAvailable())
    return false;

  auto deploymentTarget =
      AvailabilityContext::forDeploymentTarget(getASTContext());

  if (getASTContext().LangOpts.WeakLinkAtTarget)
    return !Availability.isSupersetOf(deploymentTarget);

  return !deploymentTarget.isContainedIn(Availability);
}

SILBasicBlock *SILFunction::createBasicBlock() {
  SILBasicBlock *newBlock = new (getModule()) SILBasicBlock(this);
  BlockList.push_back(newBlock);
  return newBlock;
}

SILBasicBlock *SILFunction::createBasicBlock(llvm::StringRef debugName) {
  SILBasicBlock *newBlock = new (getModule()) SILBasicBlock(this);
  newBlock->setDebugName(debugName);
  BlockList.push_back(newBlock);
  return newBlock;
}

SILBasicBlock *SILFunction::createBasicBlockAfter(SILBasicBlock *afterBB) {
  SILBasicBlock *newBlock = new (getModule()) SILBasicBlock(this);
  BlockList.insertAfter(afterBB->getIterator(), newBlock);
  return newBlock;
}

SILBasicBlock *SILFunction::createBasicBlockBefore(SILBasicBlock *beforeBB) {
  SILBasicBlock *newBlock = new (getModule()) SILBasicBlock(this);
  BlockList.insert(beforeBB->getIterator(), newBlock);
  return newBlock;
}

void SILFunction::moveAllBlocksFromOtherFunction(SILFunction *F) {
  BlockList.splice(begin(), F->BlockList);
  
  SILModule &mod = getModule();
  for (SILBasicBlock &block : *this) {
    for (SILInstruction &inst : block) {
      mod.notifyMovedInstruction(&inst, F);
    }
  }
}

void SILFunction::moveBlockFromOtherFunction(SILBasicBlock *blockInOtherFunction,
                                iterator insertPointInThisFunction) {
  SILFunction *otherFunc = blockInOtherFunction->getParent();
  assert(otherFunc != this);
  BlockList.splice(insertPointInThisFunction, otherFunc->BlockList,
                   blockInOtherFunction);

  SILModule &mod = getModule();
  for (SILInstruction &inst : *blockInOtherFunction) {
    mod.notifyMovedInstruction(&inst, otherFunc);
  }
}

void SILFunction::moveBlockBefore(SILBasicBlock *BB, SILFunction::iterator IP) {
  assert(BB->getParent() == this);
  if (SILFunction::iterator(BB) == IP)
    return;
  BlockList.remove(BB);
  BlockList.insert(IP, BB);
}

//===----------------------------------------------------------------------===//
//                          View CFG Implementation
//===----------------------------------------------------------------------===//

#ifndef NDEBUG

static llvm::cl::opt<unsigned>
MaxColumns("view-cfg-max-columns", llvm::cl::init(80),
           llvm::cl::desc("Maximum width of a printed node"));

namespace {
enum class LongLineBehavior { None, Truncate, Wrap };
} // end anonymous namespace
static llvm::cl::opt<LongLineBehavior>
LLBehavior("view-cfg-long-line-behavior",
           llvm::cl::init(LongLineBehavior::Truncate),
           llvm::cl::desc("Behavior when line width is greater than the "
                          "value provided my -view-cfg-max-columns "
                          "option"),
           llvm::cl::values(
               clEnumValN(LongLineBehavior::None, "none", "Print everything"),
               clEnumValN(LongLineBehavior::Truncate, "truncate",
                          "Truncate long lines"),
               clEnumValN(LongLineBehavior::Wrap, "wrap", "Wrap long lines")));

static llvm::cl::opt<bool>
RemoveUseListComments("view-cfg-remove-use-list-comments",
                      llvm::cl::init(false),
                      llvm::cl::desc("Should use list comments be removed"));

template <typename InstTy, typename CaseValueTy>
inline CaseValueTy getCaseValueForBB(const InstTy *Inst,
                                     const SILBasicBlock *BB) {
  for (unsigned i = 0, e = Inst->getNumCases(); i != e; ++i) {
    auto P = Inst->getCase(i);
    if (P.second != BB)
      continue;
    return P.first;
  }
  llvm_unreachable("Error! should never pass in BB that is not a successor");
}

namespace llvm {
template <>
struct DOTGraphTraits<SILFunction *> : public DefaultDOTGraphTraits {

  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getGraphName(SILFunction *F) {
    return "CFG for '" + F->getName().str() + "' function";
  }

  static std::string getSimpleNodeLabel(SILBasicBlock *Node, SILFunction *F) {
    std::string OutStr;
    raw_string_ostream OSS(OutStr);
    const_cast<SILBasicBlock *>(Node)->printAsOperand(OSS, false);
    return OSS.str();
  }

  static std::string getCompleteNodeLabel(SILBasicBlock *Node, SILFunction *F) {
    std::string Str;
    raw_string_ostream OS(Str);

    OS << *Node;
    std::string OutStr = OS.str();
    if (OutStr[0] == '\n')
      OutStr.erase(OutStr.begin());

    // Process string output to make it nicer...
    unsigned ColNum = 0;
    unsigned LastSpace = 0;
    for (unsigned i = 0; i != OutStr.length(); ++i) {
      if (OutStr[i] == '\n') { // Left justify
        OutStr[i] = '\\';
        OutStr.insert(OutStr.begin() + i + 1, 'l');
        ColNum = 0;
        LastSpace = 0;
      } else if (RemoveUseListComments && OutStr[i] == '/' &&
                 i != (OutStr.size() - 1) && OutStr[i + 1] == '/') {
        unsigned Idx = OutStr.find('\n', i + 1); // Find end of line
        OutStr.erase(OutStr.begin() + i, OutStr.begin() + Idx);
        --i;

      } else if (ColNum == MaxColumns) { // Handle long lines.

        if (LLBehavior == LongLineBehavior::Wrap) {
          if (!LastSpace)
            LastSpace = i;
          OutStr.insert(LastSpace, "\\l...");
          ColNum = i - LastSpace;
          LastSpace = 0;
          i += 3; // The loop will advance 'i' again.
        } else if (LLBehavior == LongLineBehavior::Truncate) {
          unsigned Idx = OutStr.find('\n', i + 1); // Find end of line
          OutStr.erase(OutStr.begin() + i, OutStr.begin() + Idx);
          --i;
        }

        // Else keep trying to find a space.
      } else
        ++ColNum;
      if (OutStr[i] == ' ')
        LastSpace = i;
    }
    return OutStr;
  }

  std::string getNodeLabel(SILBasicBlock *Node, SILFunction *Graph) {
    if (isSimple())
      return getSimpleNodeLabel(Node, Graph);
    else
      return getCompleteNodeLabel(Node, Graph);
  }

  static std::string getEdgeSourceLabel(SILBasicBlock *Node,
                                        SILBasicBlock::succblock_iterator I) {
    const SILBasicBlock *Succ = *I;
    const TermInst *Term = Node->getTerminator();

    // Label source of conditional branches with "T" or "F"
    if (auto *CBI = dyn_cast<CondBranchInst>(Term))
      return (Succ == CBI->getTrueBB()) ? "T" : "F";

    // Label source of switch edges with the associated value.
    if (auto *SI = dyn_cast<SwitchValueInst>(Term)) {
      if (SI->hasDefault() && SI->getDefaultBB() == Succ)
        return "def";

      std::string Str;
      raw_string_ostream OS(Str);

      SILValue I = getCaseValueForBB<SwitchValueInst, SILValue>(SI, Succ);
      OS << I; // TODO: or should we output the literal value of I?
      return OS.str();
    }

    if (auto *SEIB = dyn_cast<SwitchEnumInst>(Term)) {
      std::string Str;
      raw_string_ostream OS(Str);

      EnumElementDecl *E =
          getCaseValueForBB<SwitchEnumInst, EnumElementDecl *>(SEIB, Succ);
      OS << E->getName();
      return OS.str();
    }

    if (auto *SEIB = dyn_cast<SwitchEnumAddrInst>(Term)) {
      std::string Str;
      raw_string_ostream OS(Str);

      EnumElementDecl *E =
          getCaseValueForBB<SwitchEnumAddrInst, EnumElementDecl *>(SEIB, Succ);
      OS << E->getName();
      return OS.str();
    }

    if (auto *DMBI = dyn_cast<DynamicMethodBranchInst>(Term))
      return (Succ == DMBI->getHasMethodBB()) ? "T" : "F";

    if (auto *CCBI = dyn_cast<CheckedCastBranchInst>(Term))
      return (Succ == CCBI->getSuccessBB()) ? "T" : "F";

    if (auto *CCBI = dyn_cast<CheckedCastAddrBranchInst>(Term))
      return (Succ == CCBI->getSuccessBB()) ? "T" : "F";

    return "";
  }
};
} // namespace llvm
#endif

#ifndef NDEBUG
static llvm::cl::opt<std::string>
TargetFunction("view-cfg-only-for-function", llvm::cl::init(""),
               llvm::cl::desc("Only print out the cfg for this function"));
#endif

static void viewCFGHelper(const SILFunction* f, bool skipBBContents) {
/// When asserts are disabled, this should be a NoOp.
#ifndef NDEBUG
    // If we have a target function, only print that function out.
    if (!TargetFunction.empty() && !(f->getName().str() == TargetFunction))
      return;

    ViewGraph(const_cast<SILFunction *>(f), "cfg" + f->getName().str(),
              /*shortNames=*/skipBBContents);
#endif
}

void SILFunction::viewCFG() const {
  viewCFGHelper(this, /*skipBBContents=*/false);
}

void SILFunction::viewCFGOnly() const {
  viewCFGHelper(this, /*skipBBContents=*/true);
}


bool SILFunction::hasDynamicSelfMetadata() const {
  auto paramTypes =
      getConventions().getParameterSILTypes(TypeExpansionContext::minimal());
  if (paramTypes.empty())
    return false;

  auto silTy = *std::prev(paramTypes.end());
  if (!silTy.isObject())
    return false;

  auto selfTy = silTy.getASTType();

  if (auto metaTy = dyn_cast<MetatypeType>(selfTy)) {
    selfTy = metaTy.getInstanceType();
    if (auto dynamicSelfTy = dyn_cast<DynamicSelfType>(selfTy))
      selfTy = dynamicSelfTy.getSelfType();
  }

  if (selfTy.isForeignReferenceType())
    return false;

  return !!selfTy.getClassOrBoundGenericClass();
}

bool SILFunction::hasName(const char *Name) const {
  return getName() == Name;
}

/// Returns true if this function can be referenced from a fragile function
/// body.
bool SILFunction::hasValidLinkageForFragileRef() const {
  // Fragile functions can reference 'static inline' functions imported
  // from C.
  if (hasForeignBody())
    return true;

  // If we can inline it, we can reference it.
  if (hasValidLinkageForFragileInline())
    return true;

  // If the containing module has been serialized already, we no longer
  // enforce any invariants.
  if (getModule().isSerialized())
    return true;

  // If the function has a subclass scope that limits its visibility outside
  // the module despite its linkage, we cannot reference it.
  if (getClassSubclassScope() == SubclassScope::Resilient &&
      isAvailableExternally())
    return false;

  // Otherwise, only public functions can be referenced.
  return hasPublicVisibility(getLinkage());
}

bool
SILFunction::isPossiblyUsedExternally() const {
  auto linkage = getLinkage();

  // Hidden functions may be referenced by other C code in the linkage unit.
  if (linkage == SILLinkage::Hidden && hasCReferences())
    return true;

  if (ReplacedFunction)
    return true;

  if (isDistributed() && isThunk())
    return true;

  if (isRuntimeAccessible())
    return true;

  if (markedAsUsed())
    return true;

  // Declaration marked as `@_alwaysEmitIntoClient` that
  // returns opaque result type with availability conditions
  // has to be kept alive to emit opaque type metadata descriptor.
  if (markedAsAlwaysEmitIntoClient() &&
      hasOpaqueResultTypeWithAvailabilityConditions())
    return true;

  return swift::isPossiblyUsedExternally(linkage, getModule().isWholeModule());
}

bool SILFunction::isExternallyUsedSymbol() const {
  return swift::isPossiblyUsedExternally(getEffectiveSymbolLinkage(),
                                         getModule().isWholeModule());
}

void SILFunction::clear() {
  dropAllReferences();
  eraseAllBlocks();
}

void SILFunction::eraseAllBlocks() {
  BlockList.clear();
}

SubstitutionMap SILFunction::getForwardingSubstitutionMap() {
  if (ForwardingSubMap)
    return ForwardingSubMap;

  if (auto *env = getGenericEnvironment())
    ForwardingSubMap = env->getForwardingSubstitutionMap();

  return ForwardingSubMap;
}

bool SILFunction::shouldVerifyOwnership() const {
  return !hasSemanticsAttr("verify.ownership.sil.never");
}

static Identifier getIdentifierForObjCSelector(ObjCSelector selector, ASTContext &Ctxt) {
  SmallVector<char, 64> buffer;
  auto str = selector.getString(buffer);
  return Ctxt.getIdentifier(str);
}

void SILFunction::setObjCReplacement(AbstractFunctionDecl *replacedFunc) {
  assert(ReplacedFunction == nullptr && ObjCReplacementFor.empty());
  assert(replacedFunc != nullptr);
  ObjCReplacementFor = getIdentifierForObjCSelector(
      replacedFunc->getObjCSelector(), getASTContext());
}

void SILFunction::setObjCReplacement(Identifier replacedFunc) {
  assert(ReplacedFunction == nullptr && ObjCReplacementFor.empty());
  ObjCReplacementFor = replacedFunc;
}

// See swift/Basic/Statistic.h for declaration: this enables tracing
// SILFunctions, is defined here to avoid too much layering violation / circular
// linkage dependency.

struct SILFunctionTraceFormatter : public UnifiedStatsReporter::TraceFormatter {
  void traceName(const void *Entity, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const SILFunction *F = static_cast<const SILFunction *>(Entity);
    F->printName(OS);
  }

  void traceLoc(const void *Entity, SourceManager *SM,
                clang::SourceManager *CSM, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const SILFunction *F = static_cast<const SILFunction *>(Entity);
    if (!F->hasLocation())
      return;
    F->getLocation().getSourceRange().print(OS, *SM, false);
  }
};

static SILFunctionTraceFormatter TF;

template<>
const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const SILFunction *>() {
  return &TF;
}

bool SILFunction::hasPrespecialization() const {
  for (auto *attr : getSpecializeAttrs()) {
    if (attr->isExported())
      return true;
  }
  return false;
}

void SILFunction::forEachSpecializeAttrTargetFunction(
      llvm::function_ref<void(SILFunction *)> action) {
  for (auto *attr : getSpecializeAttrs()) {
    if (auto *f = attr->getTargetFunction()) {
      action(f);
    }
  }
}

void BridgedFunction::registerBridging(SwiftMetatype metatype,
            RegisterFn initFn, RegisterFn destroyFn,
            WriteFn writeFn, ParseFn parseFn,
            CopyEffectsFn copyEffectsFn,
            GetEffectInfoFn effectInfoFn,
            GetMemBehaviorFn memBehaviorFn) {
  functionMetatype = metatype;
  initFunction = initFn;
  destroyFunction = destroyFn;
  writeFunction = writeFn;
  parseFunction = parseFn;
  copyEffectsFunction = copyEffectsFn;
  getEffectInfoFunction = effectInfoFn;
  getMemBehvaiorFunction = memBehaviorFn;
}

std::pair<const char *, int>  SILFunction::
parseArgumentEffectsFromSource(StringRef effectStr, ArrayRef<StringRef> paramNames) {
  if (parseFunction) {
    auto error = parseFunction(
        {this}, effectStr, BridgedFunction::ParseEffectsMode::argumentEffectsFromSource, -1,
        {(const unsigned char *)paramNames.data(), paramNames.size()});
    return {(const char *)error.message, (int)error.position};
  }
  return {nullptr, 0};
}

std::pair<const char *, int>  SILFunction::
parseArgumentEffectsFromSIL(StringRef effectStr, int argumentIndex) {
  if (parseFunction) {
    auto error = parseFunction(
        {this}, effectStr, BridgedFunction::ParseEffectsMode::argumentEffectsFromSIL, argumentIndex, {nullptr, 0});
    return {(const char *)error.message, (int)error.position};
  }
  return {nullptr, 0};
}

std::pair<const char *, int>  SILFunction::parseGlobalEffectsFromSIL(StringRef effectStr) {
  if (parseFunction) {
    auto error = parseFunction(
        {this}, effectStr, BridgedFunction::ParseEffectsMode::globalEffectsFromSIL, -1, {nullptr, 0});
    return {(const char *)error.message, (int)error.position};
  }
  return {nullptr, 0};
}

std::pair<const char *, int>  SILFunction::
parseMultipleEffectsFromSIL(StringRef effectStr) {
  if (parseFunction) {
    auto error = parseFunction(
        {this}, effectStr, BridgedFunction::ParseEffectsMode::multipleEffectsFromSIL, -1, {nullptr, 0});
    return {(const char *)error.message, (int)error.position};
  }
  return {nullptr, 0};
}

void SILFunction::writeEffect(llvm::raw_ostream &OS, int effectIdx) const {
  if (writeFunction) {
    writeFunction({const_cast<SILFunction *>(this)}, {&OS}, effectIdx);
  }
}

void SILFunction::copyEffects(SILFunction *from) {
  if (copyEffectsFunction) {
    copyEffectsFunction({this}, {from});
  }
}

bool SILFunction::hasArgumentEffects() const {
  if (getEffectInfoFunction) {
    BridgedFunction f = {const_cast<SILFunction *>(this)};
    return getEffectInfoFunction(f, 0).isValid;
  }
  return false;
}

void SILFunction::
visitArgEffects(std::function<void(int, int, bool)> c) const {
  if (!getEffectInfoFunction)
    return;
    
  int idx = 0;
  BridgedFunction bridgedFn = {const_cast<SILFunction *>(this)};
  while (true) {
    BridgedFunction::EffectInfo ei = getEffectInfoFunction(bridgedFn, idx);
    if (!ei.isValid)
      return;
    if (!ei.isEmpty) {
      c(idx, ei.argumentIndex, ei.isDerived);
    }
    idx++;
  }
}

MemoryBehavior SILFunction::getMemoryBehavior(bool observeRetains) {
  if (!getMemBehvaiorFunction)
    return MemoryBehavior::MayHaveSideEffects;

  auto b = getMemBehvaiorFunction({this}, observeRetains);
  return (MemoryBehavior)b;
}
