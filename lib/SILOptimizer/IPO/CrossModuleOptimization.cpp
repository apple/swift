//===--- CrossModuleOptimization.cpp - perform cross-module-optimization --===//
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
/// An optimization which marks functions and types as inlinable or usable
/// from inline. This lets such functions be serialized (later in the pipeline),
/// which makes them available for other modules.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "cross-module-serialization-setup"
#include "swift/AST/Module.h"
#include "swift/Basic/Assertions.h"
#include "swift/IRGen/TBDGen.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/FunctionOrder.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;

/// Functions up to this (abstract) size are serialized, even if they are not
/// generic.
static llvm::cl::opt<int> CMOFunctionSizeLimit("cmo-function-size-limit",
                                               llvm::cl::init(20));

static llvm::cl::opt<bool> SerializeEverything(
    "sil-cross-module-serialize-all", llvm::cl::init(false),
    llvm::cl::desc(
        "Serialize everything when performing cross module optimization in "
        "order to investigate performance differences caused by different "
        "@inlinable, @usableFromInline choices."),
    llvm::cl::Hidden);

namespace {

/// Scans a whole module and marks functions and types as inlinable or usable
/// from inline.
class CrossModuleOptimization {
  friend class InstructionVisitor;

  llvm::DenseMap<SILType, bool> typesChecked;
  llvm::SmallPtrSet<TypeBase *, 16> typesHandled;

  SILModule &M;
  
  /// True, if CMO runs by default.
  /// In this case, serialization decisions are made very conservatively to
  /// avoid code size increase.
  bool conservative;

  /// True if CMO should serialize literally everything in the module,
  /// regardless of linkage.
  bool everything;

  typedef llvm::DenseMap<SILFunction *, bool> FunctionFlags;
  FunctionFlags canSerializeFlags;

public:
  CrossModuleOptimization(SILModule &M, bool conservative, bool everything)
    : M(M), conservative(conservative), everything(everything) { }

  void serializeFunctionsInModule(SILPassManager *manager);
  void serializeWitnessTablesInModule();
  void serializeVTablesInModule();

private:
  bool isReferenceSerializeCandidate(SILFunction *F, SILOptions options);
  bool isReferenceSerializeCandidate(SILGlobalVariable *G, SILOptions options);
  SerializedKind_t getRightSerializedKind(const SILModule &mod);
  bool isSerializedWithRightKind(const SILModule &mod, SILFunction *f);
  bool isSerializedWithRightKind(const SILModule &mod, SILGlobalVariable *g);
  bool isPackageOrPublic(SILLinkage linkage);
  bool isPackageOrPublic(AccessLevel accessLevel);

  void trySerializeFunctions(ArrayRef<SILFunction *> functions);

  bool canSerializeFunction(SILFunction *function,
                    FunctionFlags &canSerializeFlags, int maxDepth);

  bool canSerializeInstruction(SILInstruction *inst,
                    FunctionFlags &canSerializeFlags, int maxDepth);

  bool canSerializeGlobal(SILGlobalVariable *global);

  bool canSerializeType(SILType type);

  bool canUseFromInline(DeclContext *declCtxt);

  bool canUseFromInline(SILFunction *func);

  bool shouldSerialize(SILFunction *F);

  void serializeFunction(SILFunction *function,
                   const FunctionFlags &canSerializeFlags);

  void serializeInstruction(SILInstruction *inst,
                                    const FunctionFlags &canSerializeFlags);

  void serializeGlobal(SILGlobalVariable *global);

  void keepMethodAlive(SILDeclRef method);

  void makeFunctionUsableFromInline(SILFunction *F);

  void makeDeclUsableFromInline(ValueDecl *decl);

  void makeTypeUsableFromInline(CanType type);

  void makeSubstUsableFromInline(const SubstitutionMap &substs);
};

/// Visitor for making used types of an instruction inlinable.
///
/// We use the SILCloner for visiting types, though it sucks that we allocate
/// instructions just to delete them immediately. But it's better than to
/// reimplement the logic.
/// TODO: separate the type visiting logic in SILCloner from the instruction
/// creation.
class InstructionVisitor : public SILCloner<InstructionVisitor> {
  friend class SILCloner<InstructionVisitor>;
  friend class SILInstructionVisitor<InstructionVisitor>;

private:
  CrossModuleOptimization &CMS;
  SILInstruction *result = nullptr;

public:
  InstructionVisitor(SILInstruction *I, CrossModuleOptimization &CMS) :
    SILCloner(*I->getFunction()), CMS(CMS) {
    Builder.setInsertionPoint(I);
  }

  SILType remapType(SILType Ty) {
    if (Ty.hasLocalArchetype()) {
      Ty = Ty.subst(getBuilder().getModule(), Functor, Functor,
                    CanGenericSignature());
    }

    CMS.makeTypeUsableFromInline(Ty.getASTType());
    return Ty;
  }

  CanType remapASTType(CanType Ty) {
    if (Ty->hasLocalArchetype())
      Ty = Ty.subst(Functor, Functor)->getCanonicalType();

    CMS.makeTypeUsableFromInline(Ty);
    return Ty;
  }

  SubstitutionMap remapSubstitutionMap(SubstitutionMap Subs) {
    if (Subs.hasLocalArchetypes())
      Subs = Subs.subst(Functor, Functor);

    CMS.makeSubstUsableFromInline(Subs);
    return Subs;
  }

  void postProcess(SILInstruction *Orig, SILInstruction *Cloned) {
    result = Cloned;
    SILCloner<InstructionVisitor>::postProcess(Orig, Cloned);
  }

  SILValue getMappedValue(SILValue Value) { return Value; }

  SILBasicBlock *remapBasicBlock(SILBasicBlock *BB) { return BB; }

  static void makeTypesUsableFromInline(SILInstruction *I,
                                        CrossModuleOptimization &CMS) {
    InstructionVisitor visitor(I, CMS);
    visitor.visit(I);
    visitor.result->eraseFromParent();
  }
};

static bool isPackageCMOEnabled(ModuleDecl *mod) {
  return mod->isResilient() && mod->serializePackageEnabled();
}

bool CrossModuleOptimization::isPackageOrPublic(SILLinkage linkage) {
  if (isPackageCMOEnabled(M.getSwiftModule()))
    return linkage == SILLinkage::Public || linkage == SILLinkage::Package;
  return linkage == SILLinkage::Public;
}

bool CrossModuleOptimization::isPackageOrPublic(AccessLevel accessLevel) {
  if (isPackageCMOEnabled(M.getSwiftModule()))
    return accessLevel == AccessLevel::Package || accessLevel == AccessLevel::Public;
  return accessLevel == AccessLevel::Public;
}

/// Checks wither this function is [serialized_for_package] due to Package CMO
/// or [serialized] with non-package CMO. The [serialized_for_package] attribute
/// is used to indicate that a function is serialized because of Package CMO, which
/// allows loadable types in a serialized function in a resiliently built module, which
/// is otherwise illegal. It's also used to determine during SIL deserialization whether
/// loadable types in a serialized function can be allowed in the client module that
/// imports the module built with Package CMO. If the client contains a [serialized]
/// function due to `@inlinable`, funtions with [serialized_for_package] from
/// the imported module are not allowed being inlined into the client function, which
/// is the correct behavior.
bool CrossModuleOptimization::isSerializedWithRightKind(const SILModule &mod,
                                    SILFunction *f) {
  // If Package CMO is enabled in resilient mode, return
  // true if the function is [serialized] due to @inlinable
  // (or similar) or [serialized_for_package] due to this
  // optimization.
  return isPackageCMOEnabled(mod.getSwiftModule()) ? f->isAnySerialized()
                                                   : f->isSerialized();
}
bool CrossModuleOptimization::isSerializedWithRightKind(const SILModule &mod,
                                      SILGlobalVariable *g) {
  return isPackageCMOEnabled(mod.getSwiftModule()) ? g->isAnySerialized()
                                                   : g->isSerialized();
}
SerializedKind_t CrossModuleOptimization::getRightSerializedKind(const SILModule &mod) {
  return isPackageCMOEnabled(mod.getSwiftModule()) ? IsSerializedForPackage
                                                   : IsSerialized;
}

static bool isSerializeCandidate(SILFunction *F, SILOptions options) {
  auto linkage = F->getLinkage();
  // If Package CMO is enabled, besides package/public definitions,
  // we allow serializing private, hidden, or shared definitions
  // that do not contain private or hidden symbol references (and
  // their nested references). If private or internal definitions are
  // serialized, they are set to a shared linkage.
  //
  // E.g. `public func foo() { print("") }` is a public function that
  // references `print`, a shared definition which does not contain
  // any private or internal symbols, thus is serialized, which in turn
  // allows `foo` to be serialized.
  // E.g. a protocol witness method for a package protocol member is
  // set to a private linkage in SILGen. By allowing such private thunk
  // to be serialized and set to shared linkage here, functions that
  // reference the thunk can be serialized as well.
  if (isPackageCMOEnabled(F->getModule().getSwiftModule()))
    return linkage != SILLinkage::PublicExternal &&
           linkage != SILLinkage::PackageExternal &&
           linkage != SILLinkage::HiddenExternal;
  return linkage == SILLinkage::Public;
}

bool CrossModuleOptimization::isReferenceSerializeCandidate(SILFunction *F, 
                                                            SILOptions options) {
  if (isPackageCMOEnabled(F->getModule().getSwiftModule())) {
    if (isSerializedWithRightKind(F->getModule(), F))
      return true;
    return hasPublicOrPackageVisibility(F->getLinkage(),
                                        /*includePackage*/ true);
  }
  return hasPublicVisibility(F->getLinkage());
}

bool CrossModuleOptimization::isReferenceSerializeCandidate(SILGlobalVariable *G,
                                                            SILOptions options) {
  if (isPackageCMOEnabled(G->getModule().getSwiftModule())) {
    if (isSerializedWithRightKind(G->getModule(), G))
      return true;
    return hasPublicOrPackageVisibility(G->getLinkage(),
                                        /*includePackage*/ true);
  }
  return hasPublicVisibility(G->getLinkage());
}

/// Select functions in the module which should be serialized.
void CrossModuleOptimization::trySerializeFunctions(
    ArrayRef<SILFunction *> functions) {
  for (SILFunction *F : functions) {
    if (isSerializeCandidate(F, M.getOptions()) || everything) {
      if (canSerializeFunction(F, canSerializeFlags, /*maxDepth*/ 64)) {
        serializeFunction(F, canSerializeFlags);
      }
    }
  }
}

void CrossModuleOptimization::serializeFunctionsInModule(SILPassManager *manager) {
  // Reorder SIL funtions in the module bottom up so we can serialize
  // the most nested referenced functions first and avoid unnecessary
  // recursive checks.
  BasicCalleeAnalysis *BCA = manager->getAnalysis<BasicCalleeAnalysis>();
  BottomUpFunctionOrder BottomUpOrder(M, BCA);
  auto bottomUpFunctions = BottomUpOrder.getFunctions();
  trySerializeFunctions(bottomUpFunctions);
}

void CrossModuleOptimization::serializeWitnessTablesInModule() {
  if (!isPackageCMOEnabled(M.getSwiftModule()))
    return;

  for (auto &wt : M.getWitnessTables()) {
    if (wt.getSerializedKind() != getRightSerializedKind(M) &&
        hasPublicOrPackageVisibility(wt.getLinkage(), /*includePackage*/ true)) {
      auto unserializedWTMethodRange = llvm::make_filter_range(
          wt.getEntries(), [&](const SILWitnessTable::Entry &entry) {
            return entry.getKind() == SILWitnessTable::Method &&
                   entry.getMethodWitness().Witness->getSerializedKind() !=
                       getRightSerializedKind(M);
          });
      // In Package CMO, we try serializing witness thunks that
      // are private if they don't contain hidden or private
      // references. If they are serialized, they are set to
      // a shared linkage. If they can't be serialized, we set
      // the linkage to package so that the witness table itself
      // can still be serialized, thus giving a chance for entires
      // that _are_ serialized to be accessed directly.
      for (const SILWitnessTable::Entry &entry: unserializedWTMethodRange) {
        if (entry.getMethodWitness().Witness->getLinkage() == SILLinkage::Private)
          entry.getMethodWitness().Witness->setLinkage(SILLinkage::Package);
      }

      bool containsInternal = llvm::any_of(
          wt.getEntries(), [&](const SILWitnessTable::Entry &entry) {
            return entry.getKind() == SILWitnessTable::Method &&
                   !entry.getMethodWitness()
                        .Witness->hasValidLinkageForFragileRef(
                            getRightSerializedKind(M));
          });
      // FIXME: This check shouldn't be necessary but added as a caution
      // to ensure we don't serialize witness table if it contains an
      // internal entry.
      if (!containsInternal)
        wt.setSerializedKind(getRightSerializedKind(M));
    }
  }
}

void CrossModuleOptimization::serializeVTablesInModule() {
  if (!isPackageCMOEnabled(M.getSwiftModule()))
    return;

  for (const auto &vt : M.getVTables()) {
    if (vt->getSerializedKind() != getRightSerializedKind(M) &&
        vt->getClass()->getEffectiveAccess() >= AccessLevel::Package) {
       bool containsInternal =
          llvm::any_of(vt->getEntries(), [&](const SILVTableEntry &entry) {
            return !entry.getImplementation()->hasValidLinkageForFragileRef(
                getRightSerializedKind(M));
          });

      // If the entries are either serialized or have package/public
      // visibility, the vtable can be serialized; the non-serialized
      // entries can still be referenced as they have the visibility
      // outside of their defining module, and the serialized entries
      // can be directly accessed since the vtable is serialized.
      // However, if it contains a private/internal entry, we don't
      // serialize the vtable at all.
      if (!containsInternal)
        vt->setSerializedKind(getRightSerializedKind(M));
    }
  }
}

/// Recursively walk the call graph and select functions to be serialized.
///
/// The results are stored in \p canSerializeFlags and the result for \p
/// function is returned.
bool CrossModuleOptimization::canSerializeFunction(
                               SILFunction *function,
                               FunctionFlags &canSerializeFlags,
                               int maxDepth) {
  auto iter = canSerializeFlags.find(function);
  // Avoid infinite recursion in case it's a cycle in the call graph.
  if (iter != canSerializeFlags.end())
    return iter->second;

  // Temporarily set the flag to false (to avoid infinite recursion) until we set
  // it to true at the end of this function.
  canSerializeFlags[function] = false;

  if (everything) {
    canSerializeFlags[function] = true;
    return true;
  }

  if (DeclContext *funcCtxt = function->getDeclContext()) {
    if (!canUseFromInline(funcCtxt))
      return false;
  }

  if (function->isAnySerialized()) {
    canSerializeFlags[function] = true;
    return true;
  }

  if (!function->isDefinition() || function->isAvailableExternally())
    return false;

  // Avoid a stack overflow in case of a very deeply nested call graph.
  if (maxDepth <= 0)
    return false;

  // If someone adds specialization attributes to a function, it's probably the
  // developer's intention that the function is _not_ serialized.
  if (!function->getSpecializeAttrs().empty())
    return false;

  // Do the same check for the specializations of such functions.
  if (function->isSpecialization()) {
    const SILFunction *parent = function->getSpecializationInfo()->getParent();
    // Don't serialize exported (public) specializations.
    if (!parent->getSpecializeAttrs().empty() &&
        function->getLinkage() == SILLinkage::Public)
      return false;
  }

  // Ask the heuristic.
  if (!shouldSerialize(function))
    return false;

  // Check if any instruction prevents serializing the function.
  for (SILBasicBlock &block : *function) {
    for (SILInstruction &inst : block) {
      if (!canSerializeInstruction(&inst, canSerializeFlags, maxDepth)) {
        return false;
      }
    }
  }
  canSerializeFlags[function] = true;
  return true;
}

/// Returns true if \p inst can be serialized.
///
/// If \p inst is a function_ref, recursively visits the referenced function.
bool CrossModuleOptimization::canSerializeInstruction(
    SILInstruction *inst, FunctionFlags &canSerializeFlags, int maxDepth) {
  // First check if any result or operand types prevent serialization.
  for (SILValue result : inst->getResults()) {
    if (!canSerializeType(result->getType()))
      return false;
  }
  for (Operand &op : inst->getAllOperands()) {
    if (!canSerializeType(op.get()->getType()))
      return false;
  }

  if (auto *FRI = dyn_cast<FunctionRefBaseInst>(inst)) {
    SILFunction *callee = FRI->getReferencedFunctionOrNull();
    if (!callee)
      return false;

    // In conservative mode we don't want to turn non-public functions into
    // public functions, because that can increase code size. E.g. if the
    // function is completely inlined afterwards.
    // Also, when emitting TBD files, we cannot introduce a new public symbol.
    if (conservative || M.getOptions().emitTBD) {
      if (!isReferenceSerializeCandidate(callee, M.getOptions()))
        return false;
    }

    // In some project configurations imported C functions are not necessarily
    // public in their modules.
    if (conservative && callee->hasClangNode())
      return false;

    // Recursively walk down the call graph.
    if (canSerializeFunction(callee, canSerializeFlags, maxDepth - 1))
      return true;

    // In case a public/internal/private function cannot be serialized, it's
    // still possible to make them public and reference them from the serialized
    // caller function.
    // Note that shared functions can be serialized, but not used from
    // inline.
    if (!canUseFromInline(callee))
      return false;

    return true;
  }
  if (auto *GAI = dyn_cast<GlobalAddrInst>(inst)) {
    SILGlobalVariable *global = GAI->getReferencedGlobal();
    if ((conservative || M.getOptions().emitTBD) &&
        !isReferenceSerializeCandidate(global, M.getOptions())) {
      return false;
    }

    // In some project configurations imported C variables are not necessarily
    // public in their modules.
    if (conservative && global->hasClangNode())
      return false;

    return true;
  }
  if (auto *KPI = dyn_cast<KeyPathInst>(inst)) {
    bool canUse = true;
    KPI->getPattern()->visitReferencedFunctionsAndMethods(
        [&](SILFunction *func) {
          if (!canUseFromInline(func))
            canUse = false;
        },
        [&](SILDeclRef method) {
          if (method.isForeign)
            canUse = false;
          else if (isPackageCMOEnabled(method.getModuleContext())) {
            // If the referenced keypath is internal, do not
            // serialize.
            auto methodScope = method.getDecl()->getFormalAccessScope(
                nullptr,
                /*treatUsableFromInlineAsPublic*/ true);
            canUse = methodScope.isPublicOrPackage();
          }
        });
    return canUse;
  }
  if (auto *MI = dyn_cast<MethodInst>(inst)) {
    // If a class_method or witness_method is internal,
    // it can't be serialized.
    auto member = MI->getMember();
    auto canUse = !member.isForeign;
    if (canUse && isPackageCMOEnabled(member.getModuleContext())) {
      auto methodScope = member.getDecl()->getFormalAccessScope(
          nullptr,
          /*treatUsableFromInlineAsPublic*/ true);
      canUse = methodScope.isPublicOrPackage();
    }
    return canUse;
  }
  if (auto *REAI = dyn_cast<RefElementAddrInst>(inst)) {
    // In conservative mode, we don't support class field accesses of non-public
    // properties, because that would require to make the field decl public -
    // which keeps more metadata alive.
    return !conservative ||
           REAI->getField()->getEffectiveAccess() >= AccessLevel::Package;
  }
  return true;
}

bool CrossModuleOptimization::canSerializeGlobal(SILGlobalVariable *global) {
  // Check for referenced functions in the initializer.
  for (const SILInstruction &initInst : *global) {
    if (auto *FRI = dyn_cast<FunctionRefInst>(&initInst)) {
      SILFunction *referencedFunc = FRI->getReferencedFunction();
      // In conservative mode we don't want to turn non-public functions into
      // public functions, because that can increase code size. E.g. if the
      // function is completely inlined afterwards.
      // Also, when emitting TBD files, we cannot introduce a new public symbol.
      if ((conservative || M.getOptions().emitTBD) &&
          !isReferenceSerializeCandidate(referencedFunc, M.getOptions())) {
        return false;
      }

      if (!canUseFromInline(referencedFunc))
        return false;
    }
  }
  return true;
}

bool CrossModuleOptimization::canSerializeType(SILType type) {
  auto iter = typesChecked.find(type);
  if (iter != typesChecked.end())
    return iter->getSecond();

  bool success = !type.getASTType().findIf(
    [this](Type rawSubType) {
      CanType subType = rawSubType->getCanonicalType();
      if (NominalTypeDecl *subNT = subType->getNominalOrBoundGenericNominal()) {

        if (conservative && subNT->getEffectiveAccess() < AccessLevel::Package) {
          return true;
        }

        // Exclude types which are defined in an @_implementationOnly imported
        // module. Such modules are not transitively available.
        if (!canUseFromInline(subNT)) {
          return true;
        }
      }
      return false;
    });
  typesChecked[type] = success;
  return success;
}

/// Returns true if the function in \p funcCtxt could be linked statically to
/// this module.
static bool couldBeLinkedStatically(DeclContext *funcCtxt, SILModule &module) {
  if (!funcCtxt)
    return true;
  ModuleDecl *funcModule = funcCtxt->getParentModule();
  // If the function is in the same module, it's not in another module which
  // could be linked statically.
  if (module.getSwiftModule() == funcModule)
    return false;
    
  // The stdlib module is always linked dynamically.
  if (funcModule == module.getASTContext().getStdlibModule())
    return false;
    
  // Conservatively assume the function is in a statically linked module.
  return true;
}

/// Returns true if the \p declCtxt can be used from a serialized function.
bool CrossModuleOptimization::canUseFromInline(DeclContext *declCtxt) {
  if (everything)
    return true;

  if (!M.getSwiftModule()->canBeUsedForCrossModuleOptimization(declCtxt))
    return false;

  /// If we are emitting a TBD file, the TBD file only contains public symbols
  /// of this module. But not public symbols of imported modules which are
  /// statically linked to the current binary.
  /// This prevents referencing public symbols from other modules which could
  /// (potentially) linked statically. Unfortunately there is no way to find out
  /// if another module is linked statically or dynamically, so we have to be
  /// conservative here.
  if (conservative && M.getOptions().emitTBD && couldBeLinkedStatically(declCtxt, M))
    return false;
    
  return true;
}

/// Returns true if the function \p func can be used from a serialized function.
bool CrossModuleOptimization::canUseFromInline(SILFunction *function) {
  if (everything)
    return true;

  if (DeclContext *funcCtxt = function->getDeclContext()) {
    if (!canUseFromInline(funcCtxt))
      return false;
  }

  switch (function->getLinkage()) {
  case SILLinkage::PublicNonABI:
  case SILLinkage::PackageNonABI:
  case SILLinkage::HiddenExternal:
    return false;
  case SILLinkage::Shared:
    // static inline C functions
    if (!function->isDefinition() && function->hasClangNode())
      return true;
    return false;
  case SILLinkage::Public:
  case SILLinkage::Package:
  case SILLinkage::Hidden:
  case SILLinkage::Private:
  case SILLinkage::PublicExternal:
  case SILLinkage::PackageExternal:
    break;
  }
  return true;
}

/// Decide whether to serialize a function.
bool CrossModuleOptimization::shouldSerialize(SILFunction *function) {
  // Check if we already handled this function before.
  if (isSerializedWithRightKind(M, function))
    return false;

  if (everything)
    return true;

  if (function->hasSemanticsAttr("optimize.no.crossmodule"))
    return false;

  if (!conservative) {
    // The basic heuristic: serialize all generic functions, because it makes a
    // huge difference if generic functions can be specialized or not.
    if (function->getLoweredFunctionType()->isPolymorphic())
      return true;

    if (function->getLinkage() == SILLinkage::Shared)
      return true;
  }

  // If package-cmo is enabled, we don't want to limit inlining
  // or should at least increase the cap.
  if (!M.getSwiftModule()->serializePackageEnabled()) {
    // Also serialize "small" non-generic functions.
    int size = 0;
    for (SILBasicBlock &block : *function) {
      for (SILInstruction &inst : block) {
        size += (int)instructionInlineCost(inst);
        if (size >= CMOFunctionSizeLimit)
          return false;
      }
    }
  }

  return true;
}

/// Serialize \p function and recursively all referenced functions which are
/// marked in \p canSerializeFlags.
void CrossModuleOptimization::serializeFunction(SILFunction *function,
                                                const FunctionFlags &canSerializeFlags) {
  if (isSerializedWithRightKind(M, function))
    return;

  if (!canSerializeFlags.lookup(function))
    return;

  if (isPackageCMOEnabled(M.getSwiftModule())) {
    // If a private thunk (such as a protocol witness method for
    // a package protocol member) does not reference any private
    // or internal symbols, thus is serialized, it's set to shared
    // linkage, so that functions that reference the thunk can be
    // serialized as well.
    if (function->getLinkage() == SILLinkage::Private ||
        function->getLinkage() == SILLinkage::Hidden)
      function->setLinkage(SILLinkage::Shared);
  }
  function->setSerializedKind(getRightSerializedKind(M));

  for (SILBasicBlock &block : *function) {
    for (SILInstruction &inst : block) {
      InstructionVisitor::makeTypesUsableFromInline(&inst, *this);
      serializeInstruction(&inst, canSerializeFlags);
    }
  }
}

/// Prepare \p inst for serialization.
///
/// If \p inst is a function_ref, recursively visits the referenced function.
void CrossModuleOptimization::serializeInstruction(SILInstruction *inst,
                                       const FunctionFlags &canSerializeFlags) {
  // Put callees onto the worklist if they should be serialized as well.
  if (auto *FRI = dyn_cast<FunctionRefBaseInst>(inst)) {
    SILFunction *callee = FRI->getReferencedFunctionOrNull();
    assert(callee);
    if (!callee->isDefinition() || callee->isAvailableExternally())
      return;
    if (canUseFromInline(callee)) {
      if (conservative) {
        // In conservative mode, avoid making non-public functions public,
        // because that can increase code size.
        if (callee->getLinkage() == SILLinkage::Private ||
            callee->getLinkage() == SILLinkage::Hidden) {
          if (callee->getEffectiveSymbolLinkage() == SILLinkage::Public) {
            // It's a internal/private class method. There is no harm in making
            // it public, because it gets public symbol linkage anyway.
            makeFunctionUsableFromInline(callee);
          } else {
            // Treat the function like a 'shared' function, e.g. like a
            // specialization. This is better for code size than to make it
            // public, because in conservative mode we are only do this for very
            // small functions.
            callee->setLinkage(SILLinkage::Shared);
          }
        }
      } else {
        // Make the function 'public'.
        makeFunctionUsableFromInline(callee);
      }
    }
    serializeFunction(callee, canSerializeFlags);
    assert(isSerializedWithRightKind(M, callee) ||
           isPackageOrPublic(callee->getLinkage()));
    return;
  }

  if (auto *GAI = dyn_cast<GlobalAddrInst>(inst)) {
    SILGlobalVariable *global = GAI->getReferencedGlobal();
    if (canSerializeGlobal(global)) {
      serializeGlobal(global);
    }
    if (!hasPublicOrPackageVisibility(
            global->getLinkage(),
            M.getSwiftModule()->serializePackageEnabled())) {
      global->setLinkage(SILLinkage::Public);
    }
    return;
  }
  if (auto *KPI = dyn_cast<KeyPathInst>(inst)) {
    KPI->getPattern()->visitReferencedFunctionsAndMethods(
        [this](SILFunction *func) { makeFunctionUsableFromInline(func); },
        [this](SILDeclRef method) { keepMethodAlive(method); });
    return;
  }
  if (auto *MI = dyn_cast<MethodInst>(inst)) {
    keepMethodAlive(MI->getMember());
    return;
  }
  if (auto *REAI = dyn_cast<RefElementAddrInst>(inst)) {
    makeDeclUsableFromInline(REAI->getField());
  }
}

void CrossModuleOptimization::serializeGlobal(SILGlobalVariable *global) {
  if (isSerializedWithRightKind(M, global))
    return;
  for (const SILInstruction &initInst : *global) {
    if (auto *FRI = dyn_cast<FunctionRefInst>(&initInst)) {
      SILFunction *callee = FRI->getReferencedFunction();
      if (callee->isDefinition() && !callee->isAvailableExternally())
        makeFunctionUsableFromInline(callee);
    }
  }
  global->setSerializedKind(getRightSerializedKind(M));
}

void CrossModuleOptimization::keepMethodAlive(SILDeclRef method) {
  if (method.isForeign)
    return;
  // Prevent the method from dead-method elimination.
  auto *methodDecl = cast<AbstractFunctionDecl>(method.getDecl());
  M.addExternallyVisibleDecl(getBaseMethod(methodDecl));
}

void CrossModuleOptimization::makeFunctionUsableFromInline(SILFunction *function) {
  assert(canUseFromInline(function));
  if (!isAvailableExternally(function->getLinkage()) &&
      !isPackageOrPublic(function->getLinkage())) {
    function->setLinkage(SILLinkage::Public);
  }
}

/// Make a nominal type, including it's context, usable from inline.
void CrossModuleOptimization::makeDeclUsableFromInline(ValueDecl *decl) {
  if (decl->getEffectiveAccess() >= AccessLevel::Package)
    return;

  // FIXME: rdar://130456707
  // Currently not all types are visited in canSerialize* calls, sometimes
  // resulting in an internal type getting @usableFromInline, which is
  // incorrect.
  // For example, for `let q = P() as? Q`, where Q is an internal class
  // inherting a public class P, Q is not visited in the canSerialize*
  // checks, thus resulting in `@usableFromInline class Q`; this is not
  // the intended behavior in the conservative mode as it modifies AST.
  //
  // To properly fix, instruction visitor needs to be refactored to do
  // both the "canSerialize" check (that visits all types) and serialize
  // or update visibility (modify AST in non-conservative modes). 
  if (isPackageCMOEnabled(M.getSwiftModule()))
    return;

  // We must not modify decls which are defined in other modules.
  if (M.getSwiftModule() != decl->getDeclContext()->getParentModule())
    return;

  if (!isPackageOrPublic(decl->getFormalAccess()) &&
      !decl->isUsableFromInline()) {
    // Mark the nominal type as "usableFromInline".
    // TODO: find a way to do this without modifying the AST. The AST should be
    // immutable at this point.
    auto &ctx = decl->getASTContext();
    auto *attr = new (ctx) UsableFromInlineAttr(/*implicit=*/true);
    decl->getAttrs().add(attr);

    if (everything) {
      // The following does _not_ apply to the Package CMO as
      // it is only supported for the conservative mode.
      //
      // With non-package CMO, serialize vtables, their superclass
      // vtables, and make all vfunctions usable from inline.
      if (auto *classDecl = dyn_cast<ClassDecl>(decl)) {
        auto *vTable = M.lookUpVTable(classDecl);
        vTable->setSerializedKind(IsSerialized);
        for (auto &entry : vTable->getEntries()) {
          makeFunctionUsableFromInline(entry.getImplementation());
        }

        classDecl->walkSuperclasses([&](ClassDecl *superClassDecl) {
          auto *vTable = M.lookUpVTable(superClassDecl);
          if (!vTable) {
            return TypeWalker::Action::Stop;
          }
          vTable->setSerializedKind(IsSerialized);
          for (auto &entry : vTable->getEntries()) {
            makeFunctionUsableFromInline(entry.getImplementation());
          }
          return TypeWalker::Action::Continue;
        });
      }
    }
  }
  if (auto *nominalCtx = dyn_cast<NominalTypeDecl>(decl->getDeclContext())) {
    makeDeclUsableFromInline(nominalCtx);
  } else if (auto *extCtx = dyn_cast<ExtensionDecl>(decl->getDeclContext())) {
    if (auto *extendedNominal = extCtx->getExtendedNominal()) {
      makeDeclUsableFromInline(extendedNominal);
    }
  } else if (decl->getDeclContext()->isLocalContext()) {
    // TODO
  }
}

/// Ensure that the \p type is usable from serialized functions.
void CrossModuleOptimization::makeTypeUsableFromInline(CanType type) {
  if (!typesHandled.insert(type.getPointer()).second)
    return;

  if (NominalTypeDecl *NT = type->getNominalOrBoundGenericNominal()) {
    makeDeclUsableFromInline(NT);
  }

  // Also make all sub-types usable from inline.
  type.visit([this](Type rawSubType) {
    CanType subType = rawSubType->getCanonicalType();
    if (typesHandled.insert(subType.getPointer()).second) {
      if (NominalTypeDecl *subNT = subType->getNominalOrBoundGenericNominal()) {
        makeDeclUsableFromInline(subNT);
      }
    }
  });
}

/// Ensure that all replacement types of \p substs are usable from serialized
/// functions.
void CrossModuleOptimization::makeSubstUsableFromInline(
                                                const SubstitutionMap &substs) {
  for (Type replType : substs.getReplacementTypes()) {
    makeTypeUsableFromInline(replType->getCanonicalType());
  }
  for (ProtocolConformanceRef pref : substs.getConformances()) {
    if (pref.isConcrete()) {
      ProtocolConformance *concrete = pref.getConcrete();
      makeDeclUsableFromInline(concrete->getProtocol());
    }
  }
}

class CrossModuleOptimizationPass: public SILModuleTransform {
  void run() override {
    auto &M = *getModule();
    if (M.getSwiftModule()->isResilient() &&
        !M.getSwiftModule()->serializePackageEnabled())
      return;
    if (!M.isWholeModule())
      return;

    bool conservative = false;
    bool everything = SerializeEverything;
    switch (M.getOptions().CMOMode) {
      case swift::CrossModuleOptimizationMode::Off:
        break;
      case swift::CrossModuleOptimizationMode::Default:
        conservative = true;
        break;
      case swift::CrossModuleOptimizationMode::Aggressive:
        conservative = false;
        break;
      case swift::CrossModuleOptimizationMode::Everything:
        everything = true;
        break;
    }

    if (!everything &&
        M.getOptions().CMOMode == swift::CrossModuleOptimizationMode::Off) {
      return;
    }

    CrossModuleOptimization CMO(M, conservative, everything);
    CMO.serializeFunctionsInModule(PM);

    // Serialize SIL v-tables and witness-tables if package-cmo is enabled.
    CMO.serializeVTablesInModule();
    CMO.serializeWitnessTablesInModule();
  }
};

} // end anonymous namespace

SILTransform *swift::createCrossModuleOptimization() {
  return new CrossModuleOptimizationPass();
}
