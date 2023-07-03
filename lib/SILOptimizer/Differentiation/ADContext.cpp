//===--- ADContext.cpp - Differentiation Context --------------*- C++ -*---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Per-module contextual information for the differentiation transform.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "differentiation"

#include "swift/SILOptimizer/Differentiation/ADContext.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/SourceFile.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using llvm::DenseMap;
using llvm::SmallPtrSet;
using llvm::SmallVector;

namespace swift {
namespace autodiff {

//===----------------------------------------------------------------------===//
// Local helpers
//===----------------------------------------------------------------------===//

/// Given an operator name, such as '+', and a protocol, returns the '+'
/// operator. If the operator does not exist in the protocol, returns null.
static FuncDecl *findOperatorDeclInProtocol(DeclName operatorName,
                                            ProtocolDecl *protocol) {
  assert(operatorName.isOperator());
  // Find the operator requirement in the given protocol declaration.
  auto opLookup = protocol->lookupDirect(operatorName);
  for (auto *decl : opLookup) {
    if (!decl->isProtocolRequirement())
      continue;
    auto *fd = dyn_cast<FuncDecl>(decl);
    if (!fd || !fd->isStatic() || !fd->isOperator())
      continue;
    return fd;
  }
  // Not found.
  return nullptr;
}

//===----------------------------------------------------------------------===//
// ADContext methods
//===----------------------------------------------------------------------===//

ADContext::ADContext(SILModuleTransform &transform)
    : transform(transform), module(*transform.getModule()),
      passManager(*transform.getPassManager()) {}

/// Get the source file for the given `SILFunction`.
static SourceFile &getSourceFile(SILFunction *f) {
  if (f->hasLocation())
    if (auto *declContext = f->getLocation().getAsDeclContext())
      if (auto *parentSourceFile = declContext->getParentSourceFile())
        return *parentSourceFile;
  for (auto *file : f->getModule().getSwiftModule()->getFiles())
    if (auto *sourceFile = dyn_cast<SourceFile>(file))
      return *sourceFile;
  llvm_unreachable("Could not resolve SourceFile from SILFunction");
}

SynthesizedFileUnit &
ADContext::getOrCreateSynthesizedFile(SILFunction *original) {
  auto &SF = getSourceFile(original);
  return SF.getOrCreateSynthesizedFile();
}

FuncDecl *ADContext::getPlusDecl() const {
  if (!cachedPlusFn) {
    cachedPlusFn = findOperatorDeclInProtocol(astCtx.getIdentifier("+"),
                                              additiveArithmeticProtocol);
    assert(cachedPlusFn && "AdditiveArithmetic.+ not found");
  }
  return cachedPlusFn;
}

FuncDecl *ADContext::getPlusEqualDecl() const {
  if (!cachedPlusEqualFn) {
    cachedPlusEqualFn = findOperatorDeclInProtocol(astCtx.getIdentifier("+="),
                                                   additiveArithmeticProtocol);
    assert(cachedPlusEqualFn && "AdditiveArithmetic.+= not found");
  }
  return cachedPlusEqualFn;
}

AccessorDecl *ADContext::getAdditiveArithmeticZeroGetter() const {
  if (cachedZeroGetter)
    return cachedZeroGetter;
  auto zeroDeclLookup = getAdditiveArithmeticProtocol()
      ->lookupDirect(getASTContext().Id_zero);
  auto *zeroDecl = cast<VarDecl>(zeroDeclLookup.front());
  assert(zeroDecl->isProtocolRequirement());
  cachedZeroGetter = zeroDecl->getOpaqueAccessor(AccessorKind::Get);
  return cachedZeroGetter;
}

void ADContext::cleanUp() {
  // Delete all references to generated functions.
  for (auto fnRef : generatedFunctionReferences) {
    if (auto *fnRefInst =
            peerThroughFunctionConversions<FunctionRefInst>(fnRef)) {
      fnRefInst->replaceAllUsesWithUndef();
      fnRefInst->eraseFromParent();
    }
  }
  // Delete all generated functions.
  for (auto *generatedFunction : generatedFunctions) {
    LLVM_DEBUG(getADDebugStream() << "Deleting generated function "
                                  << generatedFunction->getName() << '\n');
    generatedFunction->dropAllReferences();
    transform.notifyWillDeleteFunction(generatedFunction);
    module.eraseFunction(generatedFunction);
  }
}

DifferentiableFunctionInst *ADContext::createDifferentiableFunction(
    SILBuilder &builder, SILLocation loc, IndexSubset *parameterIndices,
    IndexSubset *resultIndices, SILValue original,
    llvm::Optional<std::pair<SILValue, SILValue>> derivativeFunctions) {
  auto *dfi = builder.createDifferentiableFunction(
      loc, parameterIndices, resultIndices, original, derivativeFunctions);
  processedDifferentiableFunctionInsts.erase(dfi);
  return dfi;
}

LinearFunctionInst *ADContext::createLinearFunction(
    SILBuilder &builder, SILLocation loc, IndexSubset *parameterIndices,
    SILValue original, llvm::Optional<SILValue> transposeFunction) {
  auto *lfi = builder.createLinearFunction(loc, parameterIndices, original,
                                           transposeFunction);
  processedLinearFunctionInsts.erase(lfi);
  return lfi;
}

DifferentiableFunctionExpr *
ADContext::findDifferentialOperator(DifferentiableFunctionInst *inst) {
  return inst->getLoc().getAsASTNode<DifferentiableFunctionExpr>();
}

LinearFunctionExpr *
ADContext::findDifferentialOperator(LinearFunctionInst *inst) {
  return inst->getLoc().getAsASTNode<LinearFunctionExpr>();
}

} // end namespace autodiff
} // end namespace swift
