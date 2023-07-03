//===--- SILGenEpilog.cpp - Function epilogue emission --------------------===//
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

#include "ASTVisitor.h"
#include "SILGen.h"
#include "SILGenFunction.h"
#include "swift/AST/Types.h"
#include "swift/SIL/SILArgument.h"
#include "llvm/ADT/STLExtras.h"

using namespace swift;
using namespace Lowering;

void SILGenFunction::prepareEpilog(llvm::Optional<Type> directResultType,
                                   bool isThrowing, CleanupLocation CleanupL) {
  auto *epilogBB = createBasicBlock();

  // If we have any direct results, receive them via BB arguments.
  if (directResultType) {
    auto fnConv = F.getConventions();
    // Set NeedsReturn for indirect or direct results. This ensures that SILGen
    // emits unreachable if there is no source level return.
    NeedsReturn = !(*directResultType)->isEqual(TupleType::getEmpty(getASTContext()));
    if (NeedsReturn) {
      for (auto directResult : fnConv.getDirectSILResults()) {
        SILType resultType = F.getLoweredType(F.mapTypeIntoContext(
            fnConv.getSILType(directResult, getTypeExpansionContext())));
        // @out tuples do not get flattened in the function's return type, but
        // the epilog block expects (recursively) flattened arguments. Flatten
        // the type now.
        SmallVector<SILType, 4> worklist;
        worklist.push_back(resultType);
        while (!worklist.empty()) {
          auto ty = worklist.pop_back_val();
          if (auto tupleType = ty.getASTType()->getAs<TupleType>()) {
            assert(!fnConv.useLoweredAddresses() &&
                   "expanding tuple in non-opaque-values exit block?!");
            // Push tuple elements in reverse order (resulting in later tuple
            // elements appearing earlier in worklist) so that as the worklist
            // is drained by popping the back, arguments are created for the
            // earlier types first.
            for (auto index :
                 llvm::reverse(indices(tupleType->getElementTypes()))) {
              worklist.push_back(ty.getTupleElementType(index));
            }
          } else {
            epilogBB->createPhiArgument(ty, OwnershipKind::Owned);
          }
        }
      }
    }
  }

  ReturnDest = JumpDest(epilogBB, getCleanupsDepth(), CleanupL);

  if (isThrowing) {
    prepareRethrowEpilog(CleanupL);
  }

  if (F.getLoweredFunctionType()->isCoroutine()) {
    prepareCoroutineUnwindEpilog(CleanupL);
  }
}

void SILGenFunction::prepareRethrowEpilog(CleanupLocation cleanupLoc) {
  auto exnType = SILType::getExceptionType(getASTContext());
  SILBasicBlock *rethrowBB = createBasicBlock(FunctionSection::Postmatter);
  rethrowBB->createPhiArgument(exnType, OwnershipKind::Owned);
  ThrowDest = JumpDest(rethrowBB, getCleanupsDepth(), cleanupLoc);
}

void SILGenFunction::prepareCoroutineUnwindEpilog(CleanupLocation cleanupLoc) {
  SILBasicBlock *unwindBB = createBasicBlock(FunctionSection::Postmatter);
  CoroutineUnwindDest = JumpDest(unwindBB, getCleanupsDepth(), cleanupLoc);
}

/// View a given SILType as a type-tree under the operation of tupling and visit
/// its nodes (tuple elements) in post-order.
///
/// For convenience, the index of the type in the flattened tuple is passed to
/// the visitor.
template <typename Visit>
void visitTupleTypeTreeInPostOrder(SILType root, Visit visit) {
  struct Node {
    SILType ty;
    unsigned index;
  };
  SmallVector<std::pair<Node, unsigned>, 32> stack;
  auto tupleElementCount = [](SILType ty) -> unsigned {
    if (auto tupleType = ty.getASTType()->getAs<TupleType>())
      return tupleType->getNumElements();
    return 0;
  };
  auto tupleElement = [](SILType ty, unsigned index) -> SILType {
    return ty.getTupleElementType(index);
  };
  unsigned flattenedIndex = 0;
  stack.push_back({{root, flattenedIndex}, 0});
  while (!stack.empty()) {
    while (stack.back().second != tupleElementCount(stack.back().first.ty)) {
      auto index = stack.back().second;
      stack.back().second++;
      stack.push_back(
          {{tupleElement(stack.back().first.ty, index), flattenedIndex}, 0});
    }
    auto node = stack.pop_back_val().first;
    visit(node.ty, node.index);
    if (!node.ty.getASTType()->template is<TupleType>())
      flattenedIndex += 1;
  }
}

/// Given a list of direct results, form the direct result value.
///
/// Note that this intentionally loses any tuple sub-structure of the
/// formal result type, except in the case of @out tuples where it must be
/// preserved.
static SILValue buildReturnValue(SILGenFunction &SGF, SILLocation loc,
                                 ArrayRef<SILValue> directResults) {
  if (directResults.size() == 1)
    return directResults[0];

  auto fnConv = SGF.F.getConventions();
  if (!fnConv.useLoweredAddresses()) {
    // In opaque-values code, nested @out tuples are not flattened.  Reconstruct
    // nested tuples.
    auto resultType = SGF.F.getLoweredType(SGF.F.mapTypeIntoContext(
        fnConv.getSILResultType(SGF.getTypeExpansionContext())));
    SmallVector<llvm::Optional<SILValue>, 4> mutableDirectResult;
    for (auto result : directResults) {
      mutableDirectResult.push_back({result});
    }
    visitTupleTypeTreeInPostOrder(resultType, [&](SILType ty, unsigned index) {
      if (auto tupleTy = ty.getASTType()->getAs<TupleType>()) {
        SmallVector<SILValue, 4> elements;
        unsigned offset = 0;
        auto elementCount = tupleTy->getNumElements();
        while (elements.size() < elementCount) {
          if (mutableDirectResult[index + offset].has_value()) {
            auto val = mutableDirectResult[index + offset].value();
            elements.push_back(val);
            mutableDirectResult[index + offset].reset();
          }
          ++offset;
        }
        assert(!mutableDirectResult[index].has_value());
        auto tuple = SGF.B.createTuple(loc, ty, elements);
        mutableDirectResult[index] = tuple;
      }
    });
    assert(mutableDirectResult[0].has_value());
    return mutableDirectResult[0].value();
  }

  SmallVector<TupleTypeElt, 4> eltTypes;
  for (auto elt : directResults)
    eltTypes.push_back(elt->getType().getASTType());
  auto resultType = SILType::getPrimitiveObjectType(
    CanType(TupleType::get(eltTypes, SGF.getASTContext())));
  return SGF.B.createTuple(loc, resultType, directResults);
}

static llvm::Optional<SILLocation>
prepareForEpilogBlockEmission(SILGenFunction &SGF, SILLocation topLevel,
                              SILBasicBlock *epilogBB,
                              SmallVectorImpl<SILValue> &directResults) {
  ImplicitReturnLocation implicitReturnFromTopLevel(topLevel);

  // If the current BB we are inserting into isn't terminated, and we require a
  // return, then we
  // are not allowed to fall off the end of the function and can't reach here.
  if (SGF.NeedsReturn && SGF.B.hasValidInsertionPoint())
    SGF.B.createUnreachable(implicitReturnFromTopLevel);

  if (epilogBB->pred_empty()) {
    // If the epilog was not branched to at all, kill the BB and
    // just emit the epilog into the current BB.
    while (!epilogBB->empty())
      epilogBB->back().eraseFromParent();
    SGF.eraseBasicBlock(epilogBB);

    // If the current bb is terminated then the epilog is just unreachable.
    if (!SGF.B.hasValidInsertionPoint())
      return llvm::None;

    // We emit the epilog at the current insertion point.
    return implicitReturnFromTopLevel;
  }

  if (std::next(epilogBB->pred_begin()) == epilogBB->pred_end() &&
      !SGF.B.hasValidInsertionPoint()) {
    // If the epilog has a single predecessor and there's no current insertion
    // point to fall through from, then we can weld the epilog to that
    // predecessor BB.

    // Steal the branch argument as the return value if present.
    SILBasicBlock *pred = *epilogBB->pred_begin();
    BranchInst *predBranch = cast<BranchInst>(pred->getTerminator());
    assert(predBranch->getArgs().size() == epilogBB->args_size() &&
           "epilog predecessor arguments does not match block params");

    for (auto index : indices(predBranch->getArgs())) {
      SILValue result = predBranch->getArgs()[index];
      directResults.push_back(result);
      epilogBB->getArgument(index)->replaceAllUsesWith(result);
    }

    llvm::Optional<SILLocation> returnLoc;
    // If we are optimizing, we should use the return location from the single,
    // previously processed, return statement if any.
    if (predBranch->getLoc().is<ReturnLocation>()) {
      returnLoc = predBranch->getLoc();
    } else {
      returnLoc = implicitReturnFromTopLevel;
    }

    // Kill the branch to the now-dead epilog BB.
    pred->erase(predBranch);

    // Move any instructions from the EpilogBB to the end of the 'pred' block.
    pred->spliceAtEnd(epilogBB);

    // Finally we can erase the epilog BB.
    SGF.eraseBasicBlock(epilogBB);

    // Emit the epilog into its former predecessor.
    SGF.B.setInsertionPoint(pred);
    return returnLoc;
  }

  // Move the epilog block to the end of the ordinary section.
  auto endOfOrdinarySection = SGF.StartOfPostmatter;
  SGF.F.moveBlockBefore(epilogBB, endOfOrdinarySection);

  // Emit the epilog into the epilog bb. Its arguments are the
  // direct results.
  directResults.append(epilogBB->args_begin(), epilogBB->args_end());

  // If we are falling through from the current block, the return is implicit.
  SGF.B.emitBlock(epilogBB, implicitReturnFromTopLevel);

  // If the return location is known to be that of an already
  // processed return, use it. (This will get triggered when the
  // epilog logic is simplified.)
  //
  // Otherwise make the ret instruction part of the cleanups.
  auto cleanupLoc = CleanupLocation(topLevel);
  return cleanupLoc;
}

std::pair<llvm::Optional<SILValue>, SILLocation>
SILGenFunction::emitEpilogBB(SILLocation topLevel) {
  assert(ReturnDest.getBlock() && "no epilog bb prepared?!");
  SILBasicBlock *epilogBB = ReturnDest.getBlock();
  SmallVector<SILValue, 8> directResults;

  // Prepare the epilog block for emission. If we need to actually emit the
  // block, we return a real SILLocation. Otherwise, the epilog block is
  // actually unreachable and we can just return early.
  auto returnLoc =
      prepareForEpilogBlockEmission(*this, topLevel, epilogBB, directResults);
  if (!returnLoc.has_value()) {
    return {llvm::None, topLevel};
  }

  // Emit top-level cleanups into the epilog block.
  assert(!Cleanups.hasAnyActiveCleanups(getCleanupsDepth(),
                                        ReturnDest.getDepth()) &&
         "emitting epilog in wrong scope");

  auto cleanupLoc = CleanupLocation(topLevel);
  Cleanups.emitCleanupsForReturn(cleanupLoc, NotForUnwind);

  // Build the return value.  We don't do this if there are no direct
  // results; this can happen for void functions, but also happens when
  // prepareEpilog was asked to not add result arguments to the epilog
  // block.
  SILValue returnValue;
  if (!directResults.empty()) {
    assert(directResults.size() ==
           F.getConventions().getNumExpandedDirectSILResults(
               getTypeExpansionContext()));
    returnValue = buildReturnValue(*this, cleanupLoc, directResults);
  }

  return {returnValue, *returnLoc};
}

SILLocation SILGenFunction::
emitEpilog(SILLocation TopLevel, bool UsesCustomEpilog) {
  llvm::Optional<SILValue> maybeReturnValue;
  SILLocation returnLoc(TopLevel);
  std::tie(maybeReturnValue, returnLoc) = emitEpilogBB(TopLevel);

  SILBasicBlock *ResultBB = nullptr;
  
  if (!maybeReturnValue) {
    // Nothing to do.
  } else if (UsesCustomEpilog) {
    // If the epilog is reachable, and the caller provided an epilog, just
    // remember the block so the caller can continue it.
    ResultBB = B.getInsertionBB();
    assert(ResultBB && "Didn't have an epilog block?");
    B.clearInsertionPoint();
  } else {
    // Otherwise, if the epilog block is reachable, return the return value.
    SILValue returnValue = *maybeReturnValue;

    // Return () if no return value was given.
    if (!returnValue)
      returnValue = emitEmptyTuple(CleanupLocation(TopLevel));

    B.createReturn(returnLoc, returnValue);
  }
  
  emitRethrowEpilog(TopLevel);
  emitCoroutineUnwindEpilog(TopLevel);
  
  if (ResultBB)
    B.setInsertionPoint(ResultBB);
  
  return returnLoc;
}

static bool prepareExtraEpilog(SILGenFunction &SGF, JumpDest &dest,
                               SILLocation &loc, SILValue *arg) {
  assert(!SGF.B.hasValidInsertionPoint());

  // If we don't have a destination, we don't need to emit the epilog.
  if (!dest.isValid())
    return false;

  // If the destination isn't used, we don't need to emit the epilog.
  SILBasicBlock *epilogBB = dest.getBlock();
  auto pi = epilogBB->pred_begin(), pe = epilogBB->pred_end();
  if (pi == pe) {
    dest = JumpDest::invalid();
    SGF.eraseBasicBlock(epilogBB);
    return false;
  }

  assert(epilogBB->getNumArguments() <= 1);
  assert((epilogBB->getNumArguments() == 1) == (arg != nullptr));
  if (arg) *arg = epilogBB->args_begin()[0];

  bool reposition = true;

  // If the destination has a single branch predecessor,
  // consider emitting the epilog into it.
  SILBasicBlock *predBB = *pi;
  if (++pi == pe) {
    if (auto branch = dyn_cast<BranchInst>(predBB->getTerminator())) {
      assert(branch->getArgs().size() == epilogBB->getNumArguments());

      // Save the location and operand information from the branch,
      // then destroy it.
      loc = branch->getLoc();
      if (arg) *arg = branch->getArgs()[0];
      predBB->erase(branch);

      // Erase the rethrow block.
      SGF.eraseBasicBlock(epilogBB);
      epilogBB = predBB;
      reposition = false;
    }
  }

  // Reposition the block to the end of the postmatter section
  // unless we're emitting into a single predecessor.
  if (reposition) {
    SGF.F.moveBlockBefore(epilogBB, SGF.F.end());
  }

  SGF.B.setInsertionPoint(epilogBB);

  return true;
}

void SILGenFunction::emitRethrowEpilog(SILLocation topLevel) {
  SILValue exn;
  SILLocation throwLoc = topLevel;
  if (!prepareExtraEpilog(*this, ThrowDest, throwLoc, &exn))
    return;

  Cleanups.emitCleanupsForReturn(ThrowDest.getCleanupLocation(), IsForUnwind);

  B.createThrow(CleanupLocation(throwLoc), exn);

  ThrowDest = JumpDest::invalid();
}

void SILGenFunction::emitCoroutineUnwindEpilog(SILLocation topLevel) {
  SILLocation unwindLoc = topLevel;
  if (!prepareExtraEpilog(*this, CoroutineUnwindDest, unwindLoc, nullptr))
    return;

  Cleanups.emitCleanupsForReturn(CoroutineUnwindDest.getCleanupLocation(),
                                 IsForUnwind);

  B.createUnwind(unwindLoc);

  CoroutineUnwindDest = JumpDest::invalid();
}
