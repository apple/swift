//===- DeadFunctionElimination.cpp - Eliminate dead functions and globals -===//
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

#define DEBUG_TYPE "sil-dead-function-elimination"
#include "swift/AST/ProtocolConformance.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
using namespace swift;

STATISTIC(NumDeadFunc, "Number of dead functions eliminated");
STATISTIC(NumDeadGlobals, "Number of dead global variables eliminated");

namespace {

class DeadFunctionAndGlobalElimination {
  /// Represents a function which is implementing a vtable or witness table
  /// method.
  struct FuncImpl {
    FuncImpl(SILFunction *F, ClassDecl *Cl) : F(F), Impl(Cl) {}
    FuncImpl(SILFunction *F, RootProtocolConformance *C) : F(F), Impl(C) {}

    /// The implementing function.
    SILFunction *F;

    /// This is a class decl if we are tracking a class_method (i.e. a vtable
    /// method) and a protocol conformance if we are tracking a witness_method.
    PointerUnion<ClassDecl *, RootProtocolConformance*> Impl;
  };

  /// Stores which functions implement a vtable or witness table method.
  struct MethodInfo {

    MethodInfo(bool isWitnessMethod) :
      methodIsCalled(false), isWitnessMethod(isWitnessMethod) {}

    /// All functions which implement the method. Together with the class for
    /// which the function implements the method. In case of a witness method,
    /// the class pointer is null.
    SmallVector<FuncImpl, 8> implementingFunctions;

    /// True, if the method is called, meaning that any of it's implementations
    /// may be called.
    bool methodIsCalled;

    /// True if this is a witness method, false if it's a vtable method.
    bool isWitnessMethod;

    /// Adds an implementation of the method in a specific class.
    void addClassMethodImpl(SILFunction *F, ClassDecl *C) {
      assert(!isWitnessMethod);
      implementingFunctions.push_back(FuncImpl(F, C));
    }

    /// Adds an implementation of the method in a specific conformance.
    void addWitnessFunction(SILFunction *F, RootProtocolConformance *Conf) {
      assert(isWitnessMethod);
      implementingFunctions.push_back(FuncImpl(F, Conf));
    }
  };

  SILModule *Module;

  llvm::DenseMap<AbstractFunctionDecl *, MethodInfo *> MethodInfos;
  llvm::SpecificBumpPtrAllocator<MethodInfo> MethodInfoAllocator;

  llvm::SmallSetVector<SILFunction *, 16> Worklist;

  llvm::SmallPtrSet<void *, 32> AliveFunctionsAndTables;

  bool keepExternalWitnessTablesAlive;

  /// Checks is a function is alive, e.g. because it is visible externally.
  bool isAnchorFunction(SILFunction *F) {

    // Functions that may be used externally cannot be removed.
    if (F->isPossiblyUsedExternally())
      return true;

    if (F->getDynamicallyReplacedFunction())
      return true;

    if (F->isDynamicallyReplaceable())
      return true;

    if (F->getReferencedAdHocRequirementWitnessFunction())
      return true;

    // Don't remove pre-specialized functions. We need to preserver the
    // pre-specialization specifications from other modules.
    if (F->hasPrespecialization())
      return true;

    // ObjC functions are called through the runtime and are therefore alive
    // even if not referenced inside SIL.
    if (F->getRepresentation() == SILFunctionTypeRepresentation::ObjCMethod)
      return true;

    return false;
  }

  /// Gets or creates the MethodInfo for a vtable or witness table method.
  /// \p decl The method declaration. In case of a vtable method this is always
  ///         the most overridden method.
  MethodInfo *getMethodInfo(AbstractFunctionDecl *decl, bool isWitnessMethod) {
    MethodInfo *&entry = MethodInfos[decl];
    if (entry == nullptr) {
      entry = new (MethodInfoAllocator.Allocate()) MethodInfo(isWitnessMethod);
    }
    assert(entry->isWitnessMethod == isWitnessMethod);
    return entry;
  }

  /// Returns true if a function is marked as alive.
  bool isAlive(SILFunction *F) {
    return AliveFunctionsAndTables.count(F) != 0;
  }

  /// Returns true if a witness table is marked as alive.
  bool isAlive(SILWitnessTable *WT) {
    return AliveFunctionsAndTables.count(WT) != 0;
  }

  /// Returns true if a global variable is marked as alive.
  bool isAlive(SILGlobalVariable *global) {
    return AliveFunctionsAndTables.count(global) != 0;
  }

  /// Marks a function as alive.
  void makeAlive(SILFunction *F) {
    AliveFunctionsAndTables.insert(F);
    assert(F && "function does not exist");
    Worklist.insert(F);
  }

  /// Marks all contained functions and witness tables of a witness table as
  /// alive.
  void makeAlive(SILWitnessTable *WT) {
    if (isAvailableExternally(WT->getLinkage()) &&
        !keepExternalWitnessTablesAlive) {
      return;
    }
  
    LLVM_DEBUG(llvm::dbgs() << "    scan witness table " << WT->getName()
                            << '\n');

    AliveFunctionsAndTables.insert(WT);
    for (const SILWitnessTable::Entry &entry : WT->getEntries()) {
      switch (entry.getKind()) {
        case SILWitnessTable::Method: {

          auto methodWitness = entry.getMethodWitness();
          auto *fd = cast<AbstractFunctionDecl>(methodWitness.Requirement.
                                                getDecl());
          assert(fd == getBaseMethod(fd) &&
                 "key in witness table is overridden");
          SILFunction *F = methodWitness.Witness;
          if (F) {
            MethodInfo *MI = getMethodInfo(fd, /*isWitnessMethod*/ true);
            if (MI->methodIsCalled || !F->isDefinition())
              ensureAlive(F);
          }
        } break;

        case SILWitnessTable::AssociatedTypeProtocol: {
          ProtocolConformanceRef CRef =
             entry.getAssociatedTypeProtocolWitness().Witness;
          if (CRef.isConcrete())
            ensureAliveConformance(CRef.getConcrete());
          break;
        }
        case SILWitnessTable::BaseProtocol:
          ensureAliveConformance(entry.getBaseProtocolWitness().Witness);
          break;

        case SILWitnessTable::Invalid:
        case SILWitnessTable::AssociatedType:
          break;
      }
    }

    for (const auto &conf : WT->getConditionalConformances()) {
      if (conf.Conformance.isConcrete())
        ensureAliveConformance(conf.Conformance.getConcrete());
    }
  }

  /// Marks the \p global and all functions, which are referenced from its
  /// initializer as alive.
  void makeAlive(SILGlobalVariable *global) {
    AliveFunctionsAndTables.insert(global);
    for (const SILInstruction &initInst : *global) {
      if (auto *fRef = dyn_cast<FunctionRefInst>(&initInst))
        ensureAlive(fRef->getReferencedFunction());
    }
  }

  /// Marks the declarations referenced by a key path pattern as alive if they
  /// aren't yet.
  void
  ensureKeyPathComponentIsAlive(const KeyPathPatternComponent &component) {
    component.visitReferencedFunctionsAndMethods(
      [this](SILFunction *F) {
       ensureAlive(F);
      },
      [this](SILDeclRef method) {
        if (method.isForeign) {
          // Nothing to do here: foreign functions aren't ours to be deleting.
          // (And even if they were, they're ObjC-dispatched and thus anchored
          // already: see isAnchorFunction)
        } else {
          auto decl = cast<AbstractFunctionDecl>(method.getDecl());
          if (auto clazz = dyn_cast<ClassDecl>(decl->getDeclContext())) {
            ensureAliveClassMethod(getMethodInfo(decl, /*witness*/ false),
                                   dyn_cast<FuncDecl>(decl),
                                   clazz);
          } else if (isa<ProtocolDecl>(decl->getDeclContext())) {
            ensureAliveProtocolMethod(getMethodInfo(decl, /*witness*/ true));
          } else {
            llvm_unreachable("key path keyed by a non-class, non-protocol method");
          }
        }
      }
    );
  }

  /// Marks a function as alive if it is not alive yet.
  void ensureAlive(SILFunction *F) {
    if (!isAlive(F))
      makeAlive(F);
  }

  /// Marks a global variable as alive if it is not alive yet.
  void ensureAlive(SILGlobalVariable *global) {
    if (!isAlive(global))
      makeAlive(global);
  }

  /// Marks a witness table as alive if it is not alive yet.
  void ensureAliveConformance(const ProtocolConformance *C) {
    SILWitnessTable *WT = Module->lookUpWitnessTable(C);
    if (!WT || isAlive(WT))
      return;
    makeAlive(WT);
  }

  /// Returns true if the implementation of method \p FD in class \p ImplCl
  /// may be called when the type of the class_method's operand is \p MethodCl.
  /// Both, \p MethodCl and \p ImplCl, may by null if not known or if it's a
  /// protocol method.
  static bool canHaveSameImplementation(FuncDecl *FD, ClassDecl *MethodCl,
                                        ClassDecl *ImplCl) {
    if (!FD || !MethodCl || !ImplCl)
      return true;

    // All implementations of derived classes may be called.
    if (MethodCl->isSuperclassOf(ImplCl))
      return true;

    // Check if the method implementation is the same in a super class, i.e.
    // it is not overridden in the derived class.
    auto *Impl1 = MethodCl->findImplementingMethod(FD);
    assert(Impl1);
    auto *Impl2 = ImplCl->findImplementingMethod(FD);
    assert(Impl2);

    return Impl1 == Impl2;
  }

  /// Marks the implementing functions of the method \p FD as alive. If it is a
  /// class method, \p MethodCl is the type of the class_method instruction's
  /// operand.
  void ensureAliveClassMethod(MethodInfo *mi, FuncDecl *FD, ClassDecl *MethodCl) {
    if (mi->methodIsCalled)
      return;
    bool allImplsAreCalled = true;

    for (FuncImpl &FImpl : mi->implementingFunctions) {
      if (!isAlive(FImpl.F) &&
          canHaveSameImplementation(FD, MethodCl,
                                    FImpl.Impl.get<ClassDecl *>())) {
        makeAlive(FImpl.F);
      } else {
        allImplsAreCalled = false;
      }
    }
    if (allImplsAreCalled)
      mi->methodIsCalled = true;
  }

  /// Marks the implementing functions of the protocol method \p mi as alive.
  void ensureAliveProtocolMethod(MethodInfo *mi) {
    assert(mi->isWitnessMethod);
    if (mi->methodIsCalled)
      return;
    mi->methodIsCalled = true;
    for (FuncImpl &FImpl : mi->implementingFunctions) {
      if (auto Conf = FImpl.Impl.dyn_cast<RootProtocolConformance *>()) {
        SILWitnessTable *WT = Module->lookUpWitnessTable(Conf);
        if (!WT || isAlive(WT))
          makeAlive(FImpl.F);
      } else {
        makeAlive(FImpl.F);
      }
    }
  }

  /// Scans all references inside a function.
  void scanFunction(SILFunction *F) {

    LLVM_DEBUG(llvm::dbgs() << "    scan function " << F->getName() << '\n');

    if (auto *replacedFn = F->getDynamicallyReplacedFunction())
      ensureAlive(replacedFn);

    if (auto *adHocWitness = F->getReferencedAdHocRequirementWitnessFunction())
      ensureAlive(adHocWitness);

    // First scan all instructions of the function.
    for (SILBasicBlock &BB : *F) {
      for (SILInstruction &I : BB) {
        if (auto *WMI = dyn_cast<WitnessMethodInst>(&I)) {
          auto *funcDecl = getBaseMethod(
              cast<AbstractFunctionDecl>(WMI->getMember().getDecl()));
          MethodInfo *mi = getMethodInfo(funcDecl, /*isWitnessTable*/ true);
          ensureAliveProtocolMethod(mi);
        } else if (auto *MI = dyn_cast<MethodInst>(&I)) {
          auto *funcDecl = getBaseMethod(
              cast<AbstractFunctionDecl>(MI->getMember().getDecl()));
          assert(MI->getNumOperands() - MI->getNumTypeDependentOperands() == 1
                 && "method insts except witness_method must have 1 operand");
          ClassDecl *MethodCl = MI->getOperand(0)->getType().
                                  getClassOrBoundGenericClass();
          MethodInfo *mi = getMethodInfo(funcDecl, /*isWitnessTable*/ false);
          ensureAliveClassMethod(mi, dyn_cast<FuncDecl>(funcDecl), MethodCl);
        } else if (auto *FRI = dyn_cast<FunctionRefInst>(&I)) {
          ensureAlive(FRI->getReferencedFunction());
        } else if (auto *FRI = dyn_cast<DynamicFunctionRefInst>(&I)) {
          ensureAlive(FRI->getInitiallyReferencedFunction());
        } else if (auto *FRI = dyn_cast<PreviousDynamicFunctionRefInst>(&I)) {
          ensureAlive(FRI->getInitiallyReferencedFunction());
        } else if (auto *KPI = dyn_cast<KeyPathInst>(&I)) {
          for (auto &component : KPI->getPattern()->getComponents())
            ensureKeyPathComponentIsAlive(component);
        } else if (auto *GA = dyn_cast<GlobalAddrInst>(&I)) {
          ensureAlive(GA->getReferencedGlobal());
        } else if (auto *agi = dyn_cast<AllocGlobalInst>(&I)) {
          ensureAlive(agi->getReferencedGlobal());
        } else if (auto *GV = dyn_cast<GlobalValueInst>(&I)) {
          ensureAlive(GV->getReferencedGlobal());
        } else if (auto *HSI = dyn_cast<HasSymbolInst>(&I)) {
          SmallVector<SILFunction *, 4> fns;
          HSI->getReferencedFunctions(fns);
          for (auto fn : fns) {
            ensureAlive(fn);
          }
        }
      }
    }
  }

  /// Retrieve the visibility information from the AST.
  ///
  /// This differs from SILModule::isVisibleExternally(VarDecl *) because of
  /// it's handling of class methods. It returns true for methods whose
  /// declarations are not directly visible externally, but have been imported
  /// from another module. This ensures that entries aren't deleted from vtables
  /// imported from the stdlib.
  /// FIXME: Passes should not embed special logic for handling linkage.
  bool isVisibleExternally(const ValueDecl *decl) {
    AccessLevel access = decl->getEffectiveAccess();
    SILLinkage linkage;
    switch (access) {
    case AccessLevel::Private:
    case AccessLevel::FilePrivate:
      linkage = SILLinkage::Private;
      break;
    case AccessLevel::Internal:
      linkage = SILLinkage::Hidden;
      break;
    case AccessLevel::Package:
    case AccessLevel::Public:
    case AccessLevel::Open:
      linkage = SILLinkage::Public;
      break;
    }
    if (isPossiblyUsedExternally(linkage, Module->isWholeModule()))
      return true;

    // Special case for vtable visibility.
    if (decl->getDeclContext()->getParentModule() != Module->getSwiftModule())
      return true;

    return false;
  }

  /// Find all functions which are alive from the beginning.
  /// For example, functions which may be referenced externally.
  void findAnchors() {

    findAnchorsInTables();

    for (SILFunction &F : *Module) {
      if (isAnchorFunction(&F)) {
        LLVM_DEBUG(llvm::dbgs() << "  anchor function: " << F.getName() <<"\n");
        ensureAlive(&F);
      }

      // Make sure that functions referenced by _specialize(target: targetFun())
      // are kept alive.
      F.forEachSpecializeAttrTargetFunction(
          [this](SILFunction *targetFun) { ensureAlive(targetFun); });

      if (!F.shouldOptimize()) {
        LLVM_DEBUG(llvm::dbgs() << "  anchor a no optimization function: "
                                << F.getName() << "\n");
        ensureAlive(&F);
      }
    }
    
    for (SILGlobalVariable &global : Module->getSILGlobals()) {
      if (global.isPossiblyUsedExternally())
        ensureAlive(&global);
    }
  }

  /// The main entry point of the optimization.
  bool findAliveFunctions() {

    LLVM_DEBUG(llvm::dbgs() << "running function elimination\n");

    // Find everything which may not be eliminated, e.g. because it is accessed
    // externally.
    findAnchors();

    // The core of the algorithm: Mark functions as alive which can be reached
    // from the anchors.
    while (!Worklist.empty()) {
      SILFunction *F = Worklist.back();
      Worklist.pop_back();
      scanFunction(F);
    }

    return false;
  }

  void collectMethodImplementations() {
    // Collect vtable method implementations.
    for (auto &vTable : Module->getVTables()) {
      for (const SILVTable::Entry &entry : vTable->getEntries()) {
        // We don't need to collect destructors because we mark them as alive
        // anyway.
        if (entry.getMethod().kind == SILDeclRef::Kind::Deallocator ||
            entry.getMethod().kind == SILDeclRef::Kind::IVarDestroyer) {
          continue;
        }
        SILFunction *F = entry.getImplementation();
        auto *fd = getBaseMethod(
            cast<AbstractFunctionDecl>(entry.getMethod().getDecl()));
        MethodInfo *mi = getMethodInfo(fd, /*isWitnessTable*/ false);
        mi->addClassMethodImpl(F, vTable->getClass());
      }
    }

    // Collect witness method implementations.
    for (SILWitnessTable &WT : Module->getWitnessTableList()) {
      auto Conf = WT.getConformance();
      for (const SILWitnessTable::Entry &entry : WT.getEntries()) {
        if (entry.getKind() != SILWitnessTable::Method)
          continue;

        auto methodWitness = entry.getMethodWitness();
        auto *fd = cast<AbstractFunctionDecl>(methodWitness.Requirement.
                                              getDecl());
        assert(fd == getBaseMethod(fd) &&
               "key in witness table is overridden");
        SILFunction *F = methodWitness.Witness;
        if (!F)
          continue;

        MethodInfo *mi = getMethodInfo(fd, /*isWitnessTable*/ true);
        mi->addWitnessFunction(F, Conf);
      }
    }

    // Collect default witness method implementations.
    for (SILDefaultWitnessTable &WT : Module->getDefaultWitnessTableList()) {
      for (const SILDefaultWitnessTable::Entry &entry : WT.getEntries()) {
        if (!entry.isValid() || entry.getKind() != SILWitnessTable::Method)
          continue;

        SILFunction *F = entry. getMethodWitness().Witness;
        auto *fd = cast<AbstractFunctionDecl>(
                     entry.getMethodWitness().Requirement.getDecl());
        MethodInfo *mi = getMethodInfo(fd, /*isWitnessTable*/ true);
        mi->addWitnessFunction(F, nullptr);
      }
    }
  }

  /// Take functions reachable via vtables and witness_tables into account
  /// when computing a function liveness information.
  void findAnchorsInTables() {

    collectMethodImplementations();

    // Check vtable methods.
    for (auto &vTable : Module->getVTables()) {
      for (const SILVTable::Entry &entry : vTable->getEntries()) {
        if (entry.getMethod().kind == SILDeclRef::Kind::Deallocator ||
            entry.getMethod().kind == SILDeclRef::Kind::IVarDestroyer) {
          // Destructors are alive because they are called from swift_release
          ensureAlive(entry.getImplementation());
          continue;
        }

        SILFunction *F = entry.getImplementation();
        auto *fd = getBaseMethod(
            cast<AbstractFunctionDecl>(entry.getMethod().getDecl()));

        if (// We also have to check the method declaration's access level.
            // Needed if it's a public base method declared in another
            // compilation unit (for this we have no SILFunction).
            isVisibleExternally(fd)
            || Module->isExternallyVisibleDecl(fd)
            // Declarations are always accessible externally, so they are alive.
            || !F->isDefinition()) {
          MethodInfo *mi = getMethodInfo(fd, /*isWitnessTable*/ false);
          ensureAliveClassMethod(mi, nullptr, nullptr);
        }
      }
    }

    // Check witness table methods.
    for (SILWitnessTable &WT : Module->getWitnessTableList()) {
      ProtocolConformance *Conf = WT.getConformance();
      bool tableExternallyVisible = isVisibleExternally(Conf->getProtocol());
      // The witness table is visible from "outside". Therefore all methods
      // might be called and we mark all methods as alive.
      for (const SILWitnessTable::Entry &entry : WT.getEntries()) {
        if (entry.getKind() != SILWitnessTable::Method)
          continue;

        auto methodWitness = entry.getMethodWitness();
        auto *fd = cast<AbstractFunctionDecl>(methodWitness.Requirement.
                                              getDecl());
        assert(fd == getBaseMethod(fd) &&
               "key in witness table is overridden");
        SILFunction *F = methodWitness.Witness;
        if (!F)
          continue;

        if (!tableExternallyVisible && !Module->isExternallyVisibleDecl(fd))
          continue;

        MethodInfo *mi = getMethodInfo(fd, /*isWitnessTable*/ true);
        ensureAliveProtocolMethod(mi);
      }

      // We don't do dead witness table elimination right now. So we assume
      // that all witness tables are alive. Dead witness table elimination is
      // done in IRGen by lazily emitting witness tables.
      makeAlive(&WT);
    }

    // Check default witness methods.
    for (SILDefaultWitnessTable &WT : Module->getDefaultWitnessTableList()) {
      if (isVisibleExternally(WT.getProtocol())) {
        // The default witness table is visible from "outside". Therefore all
        // methods might be called and we mark all methods as alive.
        for (const SILDefaultWitnessTable::Entry &entry : WT.getEntries()) {
          if (!entry.isValid() || entry.getKind() != SILWitnessTable::Method)
            continue;

          auto *fd =
              cast<AbstractFunctionDecl>(
                entry.getMethodWitness().Requirement.getDecl());
          assert(fd == getBaseMethod(fd) &&
                 "key in default witness table is overridden");
          SILFunction *F = entry.getMethodWitness().Witness;
          if (!F)
            continue;

          MethodInfo *mi = getMethodInfo(fd, /*isWitnessTable*/ true);
          ensureAliveProtocolMethod(mi);
        }
      }
    }
    // Check property descriptor implementations.
    for (SILProperty &P : Module->getPropertyList()) {
      if (auto component = P.getComponent()) {
        ensureKeyPathComponentIsAlive(*component);
      }
    }
    // Check differentiability witness entries.
    for (auto &dw : Module->getDifferentiabilityWitnessList()) {
      ensureAlive(dw.getOriginalFunction());
      if (dw.getJVP())
        ensureAlive(dw.getJVP());
      if (dw.getVJP())
        ensureAlive(dw.getVJP());
    }
  }

  /// Removes all dead methods from vtables and witness tables.
  bool removeDeadEntriesFromTables() {
    bool changedTable = false;
    for (auto &vTable : Module->getVTables()) {
      vTable->removeEntries_if(
          [this, &changedTable](SILVTable::Entry &entry) -> bool {
            if (!isAlive(entry.getImplementation())) {
              LLVM_DEBUG(llvm::dbgs()
                         << "  erase dead vtable method "
                         << entry.getImplementation()->getName() << "\n");
              changedTable = true;
              return true;
            }
            return false;
          });
    }

    auto &WitnessTables = Module->getWitnessTableList();
    for (auto WI = WitnessTables.begin(), EI = WitnessTables.end(); WI != EI;) {
      SILWitnessTable *WT = &*WI;
      ++WI;
      WT->clearMethods_if([this, &changedTable]
                          (const SILWitnessTable::MethodWitness &MW) -> bool {
        if (!isAlive(MW.Witness)) {
          LLVM_DEBUG(llvm::dbgs() << "  erase dead witness method "
                                  << MW.Witness->getName() << "\n");
          changedTable = true;
          return true;
        }
        return false;
      });
    }

    auto DefaultWitnessTables = Module->getDefaultWitnessTables();
    for (auto WI = DefaultWitnessTables.begin(),
              EI = DefaultWitnessTables.end();
         WI != EI;) {
      SILDefaultWitnessTable *WT = &*WI;
      ++WI;
      WT->clearMethods_if([this, &changedTable](SILFunction *MW) -> bool {
        if (!MW)
          return false;
        if (!isAlive(MW)) {
          LLVM_DEBUG(llvm::dbgs() << "  erase dead default witness method "
                                  << MW->getName() << "\n");
          changedTable = true;
          return true;
        }
        return false;
      });
    }
    return changedTable;
  }

public:
  DeadFunctionAndGlobalElimination(SILModule *module,
                                   bool keepExternalWitnessTablesAlive) :
    Module(module),
    keepExternalWitnessTablesAlive(keepExternalWitnessTablesAlive) {}

  /// The main entry point of the optimization.
  void eliminateFunctionsAndGlobals(SILModuleTransform *DFEPass) {

    LLVM_DEBUG(llvm::dbgs() << "running dead function elimination\n");
    findAliveFunctions();

    bool changedTables = removeDeadEntriesFromTables();

    // First drop all references so that we don't get problems with non-zero
    // reference counts of dead functions.
    llvm::SmallVector<SILFunction *, 16> DeadFunctions;
    llvm::SmallVector<SILGlobalVariable *, 16> DeadGlobals;
    for (SILFunction &F : *Module) {
      if (!isAlive(&F)) {
        F.dropAllReferences();
        DeadFunctions.push_back(&F);
      }
    }
    
    for (SILGlobalVariable &global : Module->getSILGlobals()) {
      if (!isAlive(&global)) {
        global.dropAllReferences();
        DeadGlobals.push_back(&global);
      }
    }

    // Next step: delete dead witness tables.
    SILModule::WitnessTableListType &WTables = Module->getWitnessTableList();
    for (auto Iter = WTables.begin(), End = WTables.end(); Iter != End;) {
      SILWitnessTable *Wt = &*Iter;
      Iter++;
      if (!isAlive(Wt)) {
        LLVM_DEBUG(llvm::dbgs() << "  erase dead witness table "
                                << Wt->getName() << '\n');
        Module->deleteWitnessTable(Wt);
      }
    }

    // Last step: delete all dead functions.
    for (SILFunction *deadFunc : DeadFunctions) {
      LLVM_DEBUG(llvm::dbgs() << "  erase dead function " << deadFunc->getName()
                              << "\n");
      ++NumDeadFunc;
      DFEPass->notifyWillDeleteFunction(deadFunc);
      Module->eraseFunction(deadFunc);
    }
    for (SILGlobalVariable *deadGlobal : DeadGlobals) {
      ++NumDeadGlobals;
      Module->eraseGlobalVariable(deadGlobal);
    }
    
    if (changedTables)
      DFEPass->invalidateFunctionTables();
  }
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                      Pass Definition and Entry Points
//===----------------------------------------------------------------------===//

namespace {

class DeadFunctionAndGlobalEliminationPass : public SILModuleTransform {

private:
  bool isLateDFE;

public:
  DeadFunctionAndGlobalEliminationPass(bool isLateDFE) : isLateDFE(isLateDFE) {}

  void run() override {
    LLVM_DEBUG(llvm::dbgs() << "Running DeadFuncElimination\n");

    // The deserializer caches functions that it deserializes so that if it is
    // asked to deserialize that function again, it does not do extra work. This
    // causes the function's reference count to be incremented causing it to be
    // alive unnecessarily. We invalidate the SILLoaderCaches here so that we
    // can eliminate such functions.
    getModule()->invalidateSILLoaderCaches();

    DeadFunctionAndGlobalElimination deadFunctionElimination(getModule(),
                                /*keepExternalWitnessTablesAlive*/ !isLateDFE);
    deadFunctionElimination.eliminateFunctionsAndGlobals(this);
  }
};

} // end anonymous namespace

SILTransform *swift::createDeadFunctionAndGlobalElimination() {
  return new DeadFunctionAndGlobalEliminationPass(/*isLateDFE*/ false);
}

SILTransform *swift::createLateDeadFunctionAndGlobalElimination() {
  return new DeadFunctionAndGlobalEliminationPass(/*isLateDFE*/ true);
}

void swift::performSILDeadFunctionElimination(SILModule *M) {
  llvm::SmallVector<PassKind, 1> Pass =
    {PassKind::DeadFunctionAndGlobalElimination};
  auto &opts = M->getOptions();
  auto plan = SILPassPipelinePlan::getPassPipelineForKinds(opts, Pass);
  executePassPipelinePlan(M, plan);
}
