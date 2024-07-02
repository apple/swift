//===--- VTableSpecializer.cpp - Specialization of vtables ----------------===//
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
// Specialize vtables of generic classes for embedded Swift.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-vtable-specializer"

#include "llvm/ADT/SmallVector.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/SIL/OptimizationRemark.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/BasicBlockOptUtils.h"
#include "swift/SILOptimizer/Utils/ConstantFolding.h"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "swift/SILOptimizer/Utils/Generics.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/StackNesting.h"

using namespace swift;

namespace {

class VTableSpecializer : public SILModuleTransform {
  bool specializeVTables(SILModule &module);

  /// The entry point to the transformation.
  void run() override {
    SILModule &module = *getModule();

    if (!module.getOptions().EmbeddedSwift) return;

    LLVM_DEBUG(llvm::dbgs() << "***** VTableSpecializer\n");

    if (specializeVTables(module)) invalidateFunctionTables();
  }
};

}  // end anonymous namespace

static SILFunction *specializeVTableMethod(SILFunction *origMethod,
                                           SubstitutionMap subs,
                                           SILModule &module,
                                           SILTransform *transform);

static bool specializeVTablesOfSuperclasses(SILModule &module, SILTransform *transform);

static bool specializeVTablesInFunction(SILFunction &func, SILModule &module,
                                        SILTransform *transform) {
  bool changed = false;
  if (func.getLoweredFunctionType()->isPolymorphic())
    return changed;

  for (SILBasicBlock &block : func) {
    for (SILInstruction &inst : block) {
      if (auto *allocRef = dyn_cast<AllocRefInst>(&inst)) {
        changed |= (specializeVTableForType(allocRef->getType(), module,
                                            transform) != nullptr);
      } else if (auto *metatype = dyn_cast<MetatypeInst>(&inst)) {
        changed |= (specializeVTableForType(
                        metatype->getType().getInstanceTypeOfMetatype(&func),
                        module, transform) != nullptr);
      } else if (auto *cm = dyn_cast<ClassMethodInst>(&inst)) {
        changed |= specializeClassMethodInst(cm);
      }
    }
  }

  return changed;
}

bool VTableSpecializer::specializeVTables(SILModule &module) {
  bool changed = false;
  for (SILFunction &func : module) {
    changed |= specializeVTablesInFunction(func, module, this);
  }

  changed |= specializeVTablesOfSuperclasses(module, this);

  for (SILVTable *vtable : module.getVTables()) {
    if (vtable->getClass()->isGenericContext()) continue;

    for (SILVTableEntry &entry : vtable->getMutableEntries()) {
      SILFunction *method = entry.getImplementation();
      if (!method->getLoweredFunctionType()->isPolymorphic()) continue;
      
      ValueDecl *decl = entry.getMethod().getDecl();
      module.getASTContext().Diags.diagnose(
          decl->getLoc(), diag::non_final_generic_class_function);

      if (decl->getLoc().isInvalid()) {
        auto demangledName = Demangle::demangleSymbolAsString(
            method->getName(),
            Demangle::DemangleOptions::SimplifiedUIDemangleOptions());
        llvm::errs() << "in function " << demangledName << "\n";
        llvm::errs() << "in class " << vtable->getClass()->getName() << "\n";
      }
    }
  }

  return changed;
}

static bool specializeVTablesOfSuperclasses(SILVTable *vtable,
                                            SILModule &module,
                                            SILTransform *transform) {
  if (vtable->getClass()->isGenericContext() && !vtable->getClassType())
    return false;

  SILType superClassTy;
  if (SILType classTy = vtable->getClassType()) {
    superClassTy = classTy.getSuperclass();
  } else {
    if (Type superTy = vtable->getClass()->getSuperclass())
      superClassTy =
          SILType::getPrimitiveObjectType(superTy->getCanonicalType());
  }
  if (superClassTy) {
    return (specializeVTableForType(superClassTy, module, transform) !=
            nullptr);
  }

  return false;
}

static bool specializeVTablesOfSuperclasses(SILModule &module,
                                            SILTransform *transform) {
  bool changed = false;
  // The module's vtable table can grow while we are specializing superclass
  // vtables.
  for (unsigned i = 0; i < module.getVTables().size(); ++i) {
    SILVTable *vtable = module.getVTables()[i];
    specializeVTablesOfSuperclasses(vtable, module, transform);
  }
  return changed;
}

SILVTable *swift::specializeVTableForType(SILType classTy, SILModule &module,
                                SILTransform *transform) {
  CanType astType = classTy.getASTType();
  BoundGenericClassType *genClassTy = dyn_cast<BoundGenericClassType>(astType);
  if (!genClassTy) return nullptr;

  if (module.lookUpSpecializedVTable(classTy)) return nullptr;

  LLVM_DEBUG(llvm::errs() << "specializeVTableFor "
                          << genClassTy->getDecl()->getName() << ' '
                          << genClassTy->getString() << '\n');

  ClassDecl *classDecl = genClassTy->getDecl();
  SILVTable *origVtable = module.lookUpVTable(classDecl);
  if (!origVtable) {
    // This cannot occur in regular builds - only if built without wmo, which
    // can only happen in SourceKit.
    // Not ideal, but better than a SourceKit crash.
    module.getASTContext().Diags.diagnose(
        SourceLoc(), diag::cannot_specialize_class, classTy.getASTType());
    return nullptr;
  }

  SubstitutionMap subs = astType->getContextSubstitutionMap(
      classDecl->getParentModule(), classDecl);

  llvm::SmallVector<SILVTableEntry, 8> newEntries;

  for (const SILVTableEntry &entry : origVtable->getEntries()) {
    SILFunction *origMethod = entry.getImplementation();
    SILFunction *specializedMethod =
        specializeVTableMethod(origMethod, subs, module, transform);
    newEntries.push_back(SILVTableEntry(entry.getMethod(), specializedMethod,
                                        entry.getKind(),
                                        entry.isNonOverridden()));
  }

  SILVTable *vtable = SILVTable::create(module, classDecl, classTy,
                                        IsNotSerialized, newEntries);

  specializeVTablesOfSuperclasses(vtable, module, transform);

  return vtable;
}

static SILFunction *specializeVTableMethod(SILFunction *origMethod,
                                           SubstitutionMap subs,
                                           SILModule &module,
                                           SILTransform *transform) {
  LLVM_DEBUG(llvm::errs() << "specializeVTableMethod " << origMethod->getName()
                          << '\n');

  if (!origMethod->getLoweredFunctionType()->isPolymorphic()) return origMethod;

  ReabstractionInfo ReInfo(module.getSwiftModule(), module.isWholeModule(),
                           ApplySite(), origMethod, subs, IsNotSerialized,
                           /*ConvertIndirectToDirect=*/true,
                           /*dropMetatypeArgs=*/false);

  if (!ReInfo.canBeSpecialized()) {
    llvm::errs() << "Cannot specialize vtable method " << origMethod->getName()
                 << '\n';
    llvm::report_fatal_error("cannot specialize vtable method");
  }

  SILOptFunctionBuilder FunctionBuilder(*transform);

  GenericFuncSpecializer FuncSpecializer(FunctionBuilder, origMethod, subs,
                                         ReInfo, /*isMandatory=*/true);
  SILFunction *SpecializedF = FuncSpecializer.lookupSpecialization();
  if (!SpecializedF) SpecializedF = FuncSpecializer.tryCreateSpecialization();
  if (!SpecializedF || SpecializedF->getLoweredFunctionType()->hasError()) {
    llvm::errs()
        << "Cannot specialize vtable method " << origMethod->getName() << '\n'
        << "Generic class methods are not supported in embedded mode\n";
    llvm::report_fatal_error("cannot specialize vtable method");
  }

  // Link after specializing to pull in everything referenced from another
  // module in case some referenced functions have non-public linkage.
  module.linkFunction(SpecializedF, SILModule::LinkingMode::LinkAll);

  SpecializedF->setLinkage(SILLinkage::Public);
  SpecializedF->setSerializedKind(IsNotSerialized);

  return SpecializedF;
}

bool swift::specializeClassMethodInst(ClassMethodInst *cm) {
  SILFunction *f = cm->getFunction();
  SILModule &m = f->getModule();

  SILValue instance = cm->getOperand();
  SILType classTy = instance->getType();
  CanType astType = classTy.getASTType();
  BoundGenericClassType *genClassTy = dyn_cast<BoundGenericClassType>(astType);
  if (!genClassTy) return false;

  ClassDecl *classDecl = genClassTy->getDecl();
  SubstitutionMap subs = astType->getContextSubstitutionMap(
      classDecl->getParentModule(), classDecl);

  SILType funcTy = cm->getType();
  SILType substitutedType =
      funcTy.substGenericArgs(m, subs, TypeExpansionContext::minimal());

  ReabstractionInfo reInfo(substitutedType.getAs<SILFunctionType>(), cm->getMember(), m);
  reInfo.createSubstitutedAndSpecializedTypes();
  CanSILFunctionType finalFuncTy = reInfo.getSpecializedType();
  SILType finalSILTy = SILType::getPrimitiveObjectType(finalFuncTy);

  SILBuilder builder(cm);
  auto *newCM = builder.createClassMethod(cm->getLoc(), cm->getOperand(),
                                          cm->getMember(), finalSILTy);

  while (!cm->use_empty()) {
    Operand *use = *cm->use_begin();
    SILInstruction *user = use->getUser();
    ApplySite AI = ApplySite::isa(user);
    if (AI && AI.getCalleeOperand() == use) {
      replaceWithSpecializedCallee(AI, newCM, reInfo);
      AI.getInstruction()->eraseFromParent();
      continue;
    }
    llvm::errs() << "unsupported use of class method "
                 << newCM->getMember().getDecl()->getName() << " in function "
                 << newCM->getFunction()->getName() << '\n';
    llvm::report_fatal_error("unsupported class method");
  }

  return true;
}

SILTransform *swift::createVTableSpecializer() {
  return new VTableSpecializer();
}
