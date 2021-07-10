//===--- StringSwitch.cpp - Optimization of string comparisons ------------===//
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
//===----------------------------------------------------------------------===//

#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/ConstExpr.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "llvm/ADT/MapVector.h"

using namespace swift;

namespace {

//===----------------------------------------------------------------------===//
//                            StringSwitchPass
//===----------------------------------------------------------------------===//

// Optimize multiple string literal comparisons against a single String type.
// This can be used to optimize a series of string comparison ifs
// or, more commonly, a string-switch-statement.
class StringSwitchPass : public SILFunctionTransform {
  // Keep track of our current function.
  SILFunction *F;

  bool
  optimizeStringCompare(SmallVectorImpl<StringLiteralInst *> &stringLiterals,
                        SmallVectorImpl<ApplyInst *> &stringCompares,
                        SILValue subject) {
    // Set the insert point after the subject.
    SILInstruction *insertionPoint = isa<SILArgument>(subject)
                                         ? &subject->getParentBlock()->front()
                                         : subject.getDefiningInstruction();

    SILBuilder builder(std::next(insertionPoint->getIterator()));
    SILOptFunctionBuilder funcBuilder(*this);

    // Clone the string literals right after the subject so that we don't have
    // dominance errors later.
    for (auto i : indices(stringLiterals)) {
      auto *str = stringLiterals[i];
      auto newStr = builder.createStringLiteral(str->getLoc(), str->getValue(),
                                                str->getEncoding());
      stringLiterals[i] = newStr;
    }

    auto dummyLoc = F->getLocation();
    SmallVector<ValueDecl *, 1> results;
    // We'll let ObjectOutliner::replaceFindStringCall do the heavy lifting.
    builder.getASTContext().lookupInSwiftModule("_findStringSwitchCase",
                                                results);
    assert(results.size());
    if (results.size() != 1)
      return false;

    auto *funcDecl = dyn_cast<FuncDecl>(results.front());
    if (!funcDecl)
      return false;

    auto funcDeclRef = SILDeclRef(funcDecl, SILDeclRef::Kind::Func);
    auto replacementFunc = funcBuilder.getOrCreateFunction(
        dummyLoc, funcDeclRef, ForDefinition_t::NotForDefinition);
    if (!replacementFunc)
      return false;

    results.pop_back();
    // Look up "assert" to get the StaticString type.
    builder.getASTContext().lookupInSwiftModule("assert", results);
    assert(results.size());
    if (results.size() != 1)
      return false;

    funcDecl = dyn_cast<FuncDecl>(results.front());
    if (!funcDecl)
      return false;

    funcDeclRef = SILDeclRef(funcDecl, SILDeclRef::Kind::Func);
    SILFunction *assertFn = funcBuilder.getOrCreateFunction(
        dummyLoc, funcDeclRef, ForDefinition_t::NotForDefinition);
    if (!assertFn)
      return false;

    FuncDecl *arrayAllocateDecl =
        builder.getASTContext().getAllocateUninitializedArray();
    assert(arrayAllocateDecl);
    std::string allocatorMangledName =
        SILDeclRef(arrayAllocateDecl, SILDeclRef::Kind::Func).mangle();
    SILFunction *arrayAllocateFn = builder.getModule().findFunction(
        allocatorMangledName, SILLinkage::PublicExternal);
    if (!arrayAllocateFn)
      return false;

    FuncDecl *arrayFinalizeDecl =
        builder.getASTContext().getFinalizeUninitializedArray();
    std::string finalizeMangledName =
        SILDeclRef(arrayFinalizeDecl, SILDeclRef::Kind::Func).mangle();
    SILFunction *arrayFinalizeFn = builder.getModule().findFunction(
        finalizeMangledName, SILLinkage::SharedExternal);
    if (!arrayFinalizeFn)
      return false;
    builder.getModule().linkFunction(arrayFinalizeFn);

    // General variables we'll need later.
    auto wordTy = SILType::getBuiltinWordType(builder.getASTContext());
    auto byteTy = SILType::getBuiltinIntegerType(8, builder.getASTContext());
    auto boolTy = SILType::getBuiltinIntegerType(1, builder.getASTContext());
    auto i32Ty = SILType::getBuiltinIntegerType(32, builder.getASTContext());
    auto assertFnFuncTy = assertFn->getLoweredFunctionType();
    auto staticStringBaseTy =
        assertFnFuncTy->getParameters()[2].getSILStorageInterfaceType();
    auto staticStringTy = staticStringBaseTy.getObjectType();
    auto staticStringAddrTy = staticStringBaseTy.getAddressType();
    auto replacementFuncTy = replacementFunc->getLoweredFunctionType();
    auto staticStringArrayTy = replacementFuncTy->getParameters()[0]
                                   .getSILStorageInterfaceType()
                                   .getAddressType();
    auto boolObjTy = SILType::getPrimitiveObjectType(builder.getASTContext()
                                                         .getBoolDecl()
                                                         ->getDeclaredType()
                                                         ->getCanonicalType());

    // Move all the strings to the top of the function.
    for (auto *str : stringLiterals) {
      str->moveBefore(stringLiterals.front());
    }

    // Build an array of static strings.
    auto staticStringSubst =
        staticStringArrayTy.getASTType()->getContextSubstitutionMap(
            builder.getModule().getSwiftModule(),
            builder.getASTContext().getArrayDecl());
    auto size =
        builder.createIntegerLiteral(dummyLoc, wordTy, stringLiterals.size());
    auto makeArrayRef = builder.createFunctionRef(dummyLoc, arrayAllocateFn);
    auto arrayTuple =
        builder.createApply(dummyLoc, makeArrayRef, staticStringSubst, {size});
    auto arrayRawPtr = builder.createTupleExtract(dummyLoc, arrayTuple, 1);
    SILValue array = builder.createTupleExtract(dummyLoc, arrayTuple, 0);
    auto arrayAddr = builder.createPointerToAddress(dummyLoc, arrayRawPtr,
                                                    staticStringAddrTy,
                                                    /*is strct*/ true);
    auto ptrToIntName = builder.getASTContext().getIdentifier("ptrtoint_Word");
    // Add the strings literals to the array of static strings.
    size_t index = 0;
    for (auto *str : stringLiterals) {
      auto elementIndex =
          builder.createIntegerLiteral(dummyLoc, wordTy, index++);
      auto arrayIndexAddr =
          builder.createIndexAddr(dummyLoc, arrayAddr, elementIndex);
      // Build the static string.
      auto strPtr = builder.createBuiltin(dummyLoc, ptrToIntName, wordTy,
                                          SubstitutionMap(), {str});
      // TODO: better way to get the string size?
      auto strSize = builder.createIntegerLiteral(dummyLoc, wordTy,
                                                  str->getValue().size());
      auto strOpts = builder.createIntegerLiteral(dummyLoc, byteTy,
                                                  2); // See StaticString._flags
      auto staticString = builder.createStruct(dummyLoc, staticStringTy,
                                               {strPtr, strSize, strOpts});
      // Store it in the array.
      builder.createStore(dummyLoc, staticString, arrayIndexAddr,
                          StoreOwnershipQualifier::Unqualified);
    }

    // "Finalize" the array of static strings.
    auto finalizeFnRef = builder.createFunctionRef(dummyLoc, arrayFinalizeFn);
    array = builder.createApply(dummyLoc, finalizeFnRef, staticStringSubst,
                                {array});

    // Generate a call to the new comparison function.
    auto replacementRef = builder.createFunctionRef(dummyLoc, replacementFunc);
    auto cmpResultInt = builder.createApply(
        dummyLoc, replacementRef, SubstitutionMap(), {array, subject});
    auto cmpResultExtract = builder.createStructExtract(
        dummyLoc, cmpResultInt,
        builder.getASTContext().getIntDecl()->getStoredProperties()[0]);
    // `_findStringSwitchCase` returns an Int32 but for some reason
    // `select_value` promotes this to an Int64 creating issues in some cases.
    auto cmpResult =
        builder.createUncheckedBitCast(dummyLoc, cmpResultExtract, i32Ty);

    // Update all the string comparisons to compare the result of the
    // new comparison function.
    index = 0;
    std::map<StringRef, unsigned> indexMap;
    for (auto *cmp : stringCompares) {
      SILBuilder stringCompareBuilder(cmp);

      // It's possible that we will compare against two strings that are equal.
      // In this case, use the index of the other string (because that's what
      // will be returned from `_findStringSwitchCase`).
      unsigned findResultIndex;
      // If we've already counted this string, then use that index.
      if (indexMap.count(stringLiterals[index]->getValue())) {
        findResultIndex = indexMap[stringLiterals[index]->getValue()];
      } else {
        // Otherwise use the current index and record it for later.
        findResultIndex = index;
        indexMap[stringLiterals[index]->getValue()] = index;
      }
      // Either way, make sure to increment the index for the next iteration.
      index++;

      auto elementIndex = stringCompareBuilder.createIntegerLiteral(
          dummyLoc, i32Ty, findResultIndex);
      auto newCmp = stringCompareBuilder.createBuiltinBinaryFunction(
          dummyLoc, "cmp_eq", i32Ty, boolTy, {elementIndex, cmpResult});
      auto newResult =
          stringCompareBuilder.createStruct(dummyLoc, boolObjTy, {newCmp});
      cmp->replaceAllUsesWith(newResult);
      // Remove the apply so that we don't find it when we run this pass again.
      cmp->eraseFromParent();
    }

    return true;
  }

public:
  StringSwitchPass() = default;

  /// The entry point to the transformation.
  void run() override {
    F = getFunction();

    // A vector of string literals and string compares that are associated with
    // a single subject stirng.
    struct CompareSet {
      SmallVector<StringLiteralInst *, 12> stringLiterals;
      SmallVector<ApplyInst *, 12> stringCompares;
    };

    // Maps the 'subject' to a list of string-compare sets. The elements in the
    // set are vectors of string compares and string literals where the
    // dominance order is valid such that movement between the blocks (if any)
    // will not cause issue.
    llvm::MapVector<SILValue, SmallVector<CompareSet, 1>> compareMap;
    // A map where each element is all the string literals used in a series of
    // comparisons agains one "normal" (the key).
    SILValue subject;

    DominanceAnalysis *domAnalysis = PM->getAnalysis<DominanceAnalysis>();
    DominanceInfo *domInfo = domAnalysis->get(F);
    DominanceOrder domOrder(&F->front(), domInfo);

    // Iterate through all the blocks in the function using dominance order
    // so that we don't miss the first comparison because we started with the
    // seconcd comparison (hypothetically).
    while (auto *block = domOrder.getNext()) {
      domOrder.pushChildren(block);

      for (auto &inst : *block) {
        auto *apply = dyn_cast<ApplyInst>(&inst);
        if (!apply || !apply->getReferencedFunctionOrNull())
          continue;
        // Filter out the string.equals functions.
        if (!apply->getReferencedFunctionOrNull()->hasSemanticsAttr(
                semantics::STRING_EQUALS))
          continue;

        unsigned subjectArgIndex = 1;
        auto info = StringLiteralInitializerInfo::getFromCallsite(
            apply->getArgument(0));
        if (!info) {
          info = StringLiteralInitializerInfo::getFromCallsite(
              apply->getArgument(1));
          subjectArgIndex = 0;
        }
        // TODO: currently, we don't support non-ascii strings. In the future
        // we could support these strings, though.
        if (!info || !info->isAscii)
          continue;

        // Now that we know the subject's argument index, we can figure out
        // what it is.
        subject = apply->getArgument(subjectArgIndex);

        // It's possible that we have two string compares against the same
        // subject that are unrelated. For example:
        // if ... { compare1 } else { compare2 }
        // In this case, we check every set associated with our subject and if
        // one succeeds we will add it to that set.
        // Otherwise, we will create a new set.
        bool foundOthers = false;
        for (auto &otherSet : compareMap[subject]) {
          // If we've already collected one comparison and the first string
          // literal isn't dominated by the comparison call, then continue
          // allowing a new set to be added to the map.
          if (!otherSet.stringLiterals.empty() &&
              !domAnalysis->get(F)->dominates(otherSet.stringLiterals.front(),
                                              apply))
            continue;

          foundOthers = true;

          // Add this to the compareMap for its corresponding "subject".
          otherSet.stringLiterals.push_back(info.getValue().inst);
          otherSet.stringCompares.push_back(apply);
        }

        // If we haven't found any sets associated with our subject where we
        // could add this compare / literal, then create a new set associated
        // with our subject.
        if (!foundOthers) {
          CompareSet &newSet = compareMap[subject].emplace_back();
          newSet.stringLiterals.push_back(info.getValue().inst);
          newSet.stringCompares.push_back(apply);
        }
      }
    }

    // Go through the compareMap and figure out if we made any changes.
    bool madeChange = false;
    // Find all the sets that are related to a subject.
    for (auto &item : compareMap) {
      // Try to optimize each set that is related to a subject.
      for (auto &associatedSet : item.second) {
        if (associatedSet.stringCompares.size() < 2)
          continue;

        madeChange |=
            optimizeStringCompare(associatedSet.stringLiterals,
                                  associatedSet.stringCompares, item.first);
      }
    }

    // Make sure we clear the list before running again.
    compareMap.clear();
    if (madeChange)
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
};

} // end anonymous namespace

SILTransform *swift::createStringSwitchPass() { return new StringSwitchPass(); }
