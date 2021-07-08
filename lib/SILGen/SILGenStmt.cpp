//===--- SILGenStmt.cpp - Implements Lowering of ASTs -> SIL for Stmts ----===//
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

#include "ArgumentScope.h"
#include "ArgumentSource.h"
#include "Condition.h"
#include "ExecutorBreadcrumb.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "SILGen.h"
#include "Scope.h"
#include "SwitchEnumBuilder.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/ProfileCounter.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/SILArgument.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;
using namespace Lowering;

template<typename...T, typename...U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  Context.Diags.diagnose(loc,
                         diag, std::forward<U>(args)...);
}

SILBasicBlock *SILGenFunction::createBasicBlockAfter(SILBasicBlock *afterBB) {
  assert(afterBB);
  return F.createBasicBlockAfter(afterBB);
}

SILBasicBlock *SILGenFunction::createBasicBlockBefore(SILBasicBlock *beforeBB) {
  assert(beforeBB);
  return F.createBasicBlockBefore(beforeBB);
}

SILBasicBlock *SILGenFunction::createBasicBlock() {
  // If we have a current insertion point, insert there.
  if (B.hasValidInsertionPoint()) {
    return F.createBasicBlockAfter(B.getInsertionBB());

  // Otherwise, insert at the end of the current section.
  } else {
    return createBasicBlock(CurFunctionSection);
  }
}

SILBasicBlock *SILGenFunction::createBasicBlock(FunctionSection section) {
  switch (section) {
  case FunctionSection::Ordinary: {
    // The end of the ordinary section is just the end of the function
    // unless postmatter blocks exist.
    if (StartOfPostmatter != F.end()) {
      return F.createBasicBlockBefore(&*StartOfPostmatter);
    } else {
      return F.createBasicBlock();
    }
  }

  case FunctionSection::Postmatter: {
    // The end of the postmatter section is always the end of the function.
    // Register the new block as the start of the postmatter if needed.
    SILBasicBlock *newBB = F.createBasicBlock();
    if (StartOfPostmatter == F.end())
      StartOfPostmatter = newBB->getIterator();
    return newBB;
  }

  }
  llvm_unreachable("bad function section");
}

SILBasicBlock *
SILGenFunction::createBasicBlockAndBranch(SILLocation loc,
                                          SILBasicBlock *destBB) {
  auto *newBB = createBasicBlock();
  SILGenBuilder(B, newBB).createBranch(loc, destBB);
  return newBB;
}

void SILGenFunction::eraseBasicBlock(SILBasicBlock *block) {
  assert(block->pred_empty() && "erasing block with predecessors");
  assert(block->empty() && "erasing block with content");
  SILFunction::iterator blockIt = block->getIterator();
  if (blockIt == StartOfPostmatter) {
    StartOfPostmatter = next_or_end(blockIt, F.end());
  }
  block->eraseFromParent();
}

// Merge blocks during a single traversal of the block list. Only unconditional
// branch edges are visited. Consequently, this takes only as much time as a
// linked list traversal and requires no additional storage.
//
// For each block, check if it can be merged with its successor. Place the
// merged block at the successor position in the block list.
//
// Typically, the successor occurs later in the list. This is most efficient
// because merging moves instructions from the successor to the
// predecessor. This way, instructions will only be moved once. Furthermore, the
// merged block will be visited again to determine if it can be merged with it's
// successor, and so on, so no edges are skipped.
//
// In rare cases, the predessor is merged with its earlier successor, which has
// already been visited. If the successor can also be merged, then it has
// already happened, and there is no need to revisit the merged block.
void SILGenFunction::mergeCleanupBlocks() {
  for (auto bbPos = F.begin(), bbEnd = F.end(), nextPos = bbPos; bbPos != bbEnd;
       bbPos = nextPos) {
    // A forward iterator refering to the next unprocessed block in the block
    // list. If blocks are merged and moved, then this will be updated.
    nextPos = std::next(bbPos);

    // Consider the current block as the predecessor.
    auto *predBB = &*bbPos;
    auto *BI = dyn_cast<BranchInst>(predBB->getTerminator());
    if (!BI)
      continue;

    // predBB has an unconditional branch to succBB. If succBB has no other
    // predecessors, then merge the blocks.
    auto *succBB = BI->getDestBB();
    if (!succBB->getSinglePredecessorBlock())
      continue;

    // Before merging, establish iterators that won't be invalidated by erasing
    // succBB. Use a reverse iterator to remember the position before a block.
    //
    // Remember the block before the current successor as a position for placing
    // the merged block.
    auto beforeSucc = std::next(SILFunction::reverse_iterator(succBB));

    // Remember the position before the current predecessor to avoid skipping
    // blocks or revisiting blocks unnecessarilly.
    auto beforePred = std::next(SILFunction::reverse_iterator(predBB));
    // Since succBB will be erased, move before it.
    if (beforePred == SILFunction::reverse_iterator(succBB))
      ++beforePred;

    // Merge `predBB` with `succBB`. This erases `succBB`.
    mergeBasicBlockWithSingleSuccessor(predBB, succBB);

    // If predBB is first in the list, then it must be the entry block which
    // cannot be moved.
    if (beforePred != F.rend()) {
      // Move the merged block into the successor position. (If the blocks are
      // not already adjacent, then the first is typically the trampoline.)
      assert(beforeSucc != F.rend()
             && "entry block cannot have a predecessor.");
      F.moveBlockAfter(predBB, &*beforeSucc);
    }
    // If after moving predBB there are no more blocks to process, then break.
    if (beforePred == F.rbegin())
      break;

    // Update the loop iterator to the next unprocessed block.
    nextPos = SILFunction::iterator(&*std::prev(beforePred));
  }
}

//===----------------------------------------------------------------------===//
// SILGenFunction emitStmt implementation
//===----------------------------------------------------------------------===//

namespace {
  class StmtEmitter : public Lowering::ASTVisitor<StmtEmitter> {
    SILGenFunction &SGF;
  public:
    StmtEmitter(SILGenFunction &sgf) : SGF(sgf) {}
#define STMT(ID, BASE) void visit##ID##Stmt(ID##Stmt *S);
#include "swift/AST/StmtNodes.def"

    void visitAsyncForEachStmt(ForEachStmt *S);

    ASTContext &getASTContext() { return SGF.getASTContext(); }

    SILBasicBlock *createBasicBlock() { return SGF.createBasicBlock(); }

    template <class... Args>
    JumpDest createJumpDest(Stmt *cleanupLoc, Args... args) {
      return JumpDest(SGF.createBasicBlock(args...),
                      SGF.getCleanupsDepth(),
                      CleanupLocation(cleanupLoc));
    }
  };
} // end anonymous namespace

void SILGenFunction::emitStmt(Stmt *S) {
  StmtEmitter(*this).visit(S);
}

/// getOrEraseBlock - If there are branches to the specified JumpDest,
/// return the block, otherwise return NULL. The JumpDest must be valid.
static SILBasicBlock *getOrEraseBlock(SILGenFunction &SGF, JumpDest &dest) {
  SILBasicBlock *BB = dest.takeBlock();
  if (BB->pred_empty()) {
    // If the block is unused, we don't need it; just delete it.
    SGF.eraseBasicBlock(BB);
    return nullptr;
  }
  return BB;
}

/// emitOrDeleteBlock - If there are branches to the specified JumpDest,
/// emit it per emitBlock.  If there aren't, then just delete the block - it
/// turns out to have not been needed.
static void emitOrDeleteBlock(SILGenFunction &SGF, JumpDest &dest,
                              SILLocation BranchLoc) {
  // If we ever add a single-use optimization here (to just continue
  // the predecessor instead of branching to a separate block), we'll
  // need to update visitDoCatchStmt so that code like:
  //   try { throw x } catch _ { }
  // doesn't leave us emitting the rest of the function in the
  // postmatter section.
  SILBasicBlock *BB = getOrEraseBlock(SGF, dest);
  if (BB != nullptr)
    SGF.B.emitBlock(BB, BranchLoc);
}

Condition SILGenFunction::emitCondition(Expr *E, bool invertValue,
                                        ArrayRef<SILType> contArgs,
                                        ProfileCounter NumTrueTaken,
                                        ProfileCounter NumFalseTaken) {
  assert(B.hasValidInsertionPoint() &&
         "emitting condition at unreachable point");

  // Sema forces conditions to have Bool type, which guarantees this.
  SILValue V;
  {
    FullExpr Scope(Cleanups, CleanupLocation(E));
    V = emitRValue(E).forwardAsSingleValue(*this, E);
  }
  auto i1Value = emitUnwrapIntegerResult(E, V);
  return emitCondition(i1Value, E, invertValue, contArgs, NumTrueTaken,
                       NumFalseTaken);
}

Condition SILGenFunction::emitCondition(SILValue V, SILLocation Loc,
                                        bool invertValue,
                                        ArrayRef<SILType> contArgs,
                                        ProfileCounter NumTrueTaken,
                                        ProfileCounter NumFalseTaken) {
  assert(B.hasValidInsertionPoint() &&
         "emitting condition at unreachable point");

  SILBasicBlock *ContBB = createBasicBlock();

  for (SILType argTy : contArgs) {
    ContBB->createPhiArgument(argTy, OwnershipKind::Owned);
  }

  SILBasicBlock *FalseBB = createBasicBlock();
  SILBasicBlock *TrueBB = createBasicBlock();

  if (invertValue)
    B.createCondBranch(Loc, V, FalseBB, TrueBB, NumFalseTaken, NumTrueTaken);
  else
    B.createCondBranch(Loc, V, TrueBB, FalseBB, NumTrueTaken, NumFalseTaken);

  return Condition(TrueBB, FalseBB, ContBB, Loc);
}

void StmtEmitter::visitBraceStmt(BraceStmt *S) {
  // Enter a new scope.
  LexicalScope BraceScope(SGF, CleanupLocation(S));
  // Keep in sync with DiagnosticsSIL.def.
  const unsigned ReturnStmtType   = 0;
  const unsigned BreakStmtType    = 1;
  const unsigned ContinueStmtType = 2;
  const unsigned ThrowStmtType    = 3;
  const unsigned UnknownStmtType  = 4;
  unsigned StmtType = UnknownStmtType;
  
  for (auto &ESD : S->getElements()) {
    
    if (auto D = ESD.dyn_cast<Decl*>())
      if (isa<IfConfigDecl>(D))
        continue;
    
    // If we ever reach an unreachable point, stop emitting statements and issue
    // an unreachable code diagnostic.
    if (!SGF.B.hasValidInsertionPoint()) {
      // If this is an implicit statement or expression, just skip over it,
      // don't emit a diagnostic here.
      if (auto *S = ESD.dyn_cast<Stmt*>()) {
        // Return statement in a single-expression closure or function is
        // implicit, but the result isn't. So, skip over return statements
        // that are implicit and either have no results or the result is
        // implicit. Otherwise, don't so we can emit unreachable code
        // diagnostics.
        if (S->isImplicit() && isa<ReturnStmt>(S)) {
          auto returnStmt = cast<ReturnStmt>(S);
          if (!returnStmt->hasResult()) {
            continue;
          }
          if (returnStmt->getResult()->isImplicit()) {
            continue;
          }
        }
        if (S->isImplicit() && !isa<ReturnStmt>(S)) {
          continue;
        }
      } else if (auto *E = ESD.dyn_cast<Expr*>()) {
        // Optional chaining expressions are wrapped in a structure like.
        //
        // (optional_evaluation_expr implicit type='T?'
        //   (call_expr type='T?'
        //     (exprs...
        //
        // Walk through it to find out if the statement is actually implicit.
        if (auto *OEE = dyn_cast<OptionalEvaluationExpr>(E)) {
          if (auto *IIO = dyn_cast<InjectIntoOptionalExpr>(OEE->getSubExpr()))
            if (IIO->getSubExpr()->isImplicit()) continue;
          if (auto *C = dyn_cast<CallExpr>(OEE->getSubExpr()))
            if (C->isImplicit()) continue;
        } else if (E->isImplicit()) {
          // Ignore all other implicit expressions.
          continue;
        }
      } else if (auto D = ESD.dyn_cast<Decl*>()) {
        // Local type declarations are not unreachable because they can appear
        // after the declared type has already been used.
        if (isa<TypeDecl>(D))
          continue;
      }
      
      if (StmtType != UnknownStmtType) {
        diagnose(getASTContext(), ESD.getStartLoc(),
                 diag::unreachable_code_after_stmt, StmtType);
      } else {
        diagnose(getASTContext(), ESD.getStartLoc(),
                 diag::unreachable_code);
        if (!S->getElements().empty()) {
          for (auto *arg : SGF.getFunction().getArguments()) {
            if (arg->getType().getASTType()->isStructurallyUninhabited()) {
              diagnose(getASTContext(), S->getStartLoc(),
                       diag::unreachable_code_uninhabited_param_note,
                       arg->getDecl()->getBaseName().userFacingName());
              break;
            }
          }
        }
      }
      return;
    }

    // Process children.
    if (auto *S = ESD.dyn_cast<Stmt*>()) {
      visit(S);
      if (isa<ReturnStmt>(S))
        StmtType = ReturnStmtType;
      if (isa<BreakStmt>(S))
        StmtType = BreakStmtType;
      if (isa<ContinueStmt>(S))
        StmtType = ContinueStmtType;
      if (isa<ThrowStmt>(S))
        StmtType = ThrowStmtType;
    } else if (auto *E = ESD.dyn_cast<Expr*>()) {
      if (auto AE = dyn_cast<AssignExpr>(E)) {
        auto dest = AE->getDest()->getSemanticsProvidingExpr();
        if (isa<DiscardAssignmentExpr>(dest)) {
          if (isa<LoadExpr>(AE->getSrc()) &&
              !AE->getDest()->getType()->hasLValueType()) {
            // Emit the destination to force the load
            SGF.emitIgnoredExpr(AE->getDest());
          }
          SGF.emitIgnoredExpr(AE->getSrc(), true);
          continue;
        }
      }
      SGF.emitIgnoredExpr(E);
    } else {
      auto *D = ESD.get<Decl*>();

      // Hoisted declarations are emitted at the top level by emitSourceFile().
      if (D->isHoisted())
        continue;

      SGF.visit(D);
    }
  }
}

namespace {
  class StoreResultInitialization : public Initialization {
    SILValue &Storage;
    SmallVectorImpl<CleanupHandle> &Cleanups;
  public:
    StoreResultInitialization(SILValue &storage,
                              SmallVectorImpl<CleanupHandle> &cleanups)
      : Storage(storage), Cleanups(cleanups) {}

    void copyOrInitValueInto(SILGenFunction &SGF, SILLocation loc,
                             ManagedValue value, bool isInit) override {
      Storage = value.getValue();
      auto cleanup = value.getCleanup();
      if (cleanup.isValid()) Cleanups.push_back(cleanup);
    }
  };
} // end anonymous namespace

static InitializationPtr
prepareIndirectResultInit(SILGenFunction &SGF,
                          CanSILFunctionType fnTypeForResults,
                          CanType resultType,
                          ArrayRef<SILResultInfo> &allResults,
                          MutableArrayRef<SILValue> &directResults,
                          ArrayRef<SILArgument*> &indirectResultAddrs,
                          SmallVectorImpl<CleanupHandle> &cleanups) {
  // Recursively decompose tuple types.
  if (auto resultTupleType = dyn_cast<TupleType>(resultType)) {
    auto tupleInit = new TupleInitialization();
    tupleInit->SubInitializations.reserve(resultTupleType->getNumElements());

    for (auto resultEltType : resultTupleType.getElementTypes()) {
      auto eltInit = prepareIndirectResultInit(SGF, fnTypeForResults,
                                               resultEltType, allResults,
                                               directResults,
                                               indirectResultAddrs, cleanups);
      tupleInit->SubInitializations.push_back(std::move(eltInit));
    }

    return InitializationPtr(tupleInit);
  }

  // Okay, pull the next result off the list of results.
  auto result = allResults[0];
  allResults = allResults.slice(1);

  // If it's indirect, we should be emitting into an argument.
  if (SGF.silConv.isSILIndirect(result)) {
    // Pull off the next indirect result argument.
    SILValue addr = indirectResultAddrs.front();
    indirectResultAddrs = indirectResultAddrs.slice(1);

    // Create an initialization which will initialize it.
    auto &resultTL = SGF.getTypeLowering(addr->getType());
    auto temporary = SGF.useBufferAsTemporary(addr, resultTL);

    // Remember the cleanup that will be activated.
    auto cleanup = temporary->getInitializedCleanup();
    if (cleanup.isValid())
      cleanups.push_back(cleanup);

    return InitializationPtr(temporary.release());
  }

  // Otherwise, make an Initialization that stores the value in the
  // next element of the directResults array.
  auto init = new StoreResultInitialization(directResults[0], cleanups);
  directResults = directResults.slice(1);
  return InitializationPtr(init);
}

/// Prepare an Initialization that will initialize the result of the
/// current function.
///
/// \param directResultsBuffer - will be filled with the direct
///   components of the result
/// \param cleanups - will be filled (after initialization completes)
///   with all the active cleanups managing the result values
std::unique_ptr<Initialization>
SILGenFunction::prepareIndirectResultInit(CanType formalResultType,
                                 SmallVectorImpl<SILValue> &directResultsBuffer,
                                 SmallVectorImpl<CleanupHandle> &cleanups) {
  auto fnConv = F.getConventions();

  // Make space in the direct-results array for all the entries we need.
  directResultsBuffer.append(fnConv.getNumDirectSILResults(), SILValue());

  ArrayRef<SILResultInfo> allResults = fnConv.funcTy->getResults();
  MutableArrayRef<SILValue> directResults = directResultsBuffer;
  ArrayRef<SILArgument*> indirectResultAddrs = F.getIndirectResults();

  auto init = ::prepareIndirectResultInit(*this,
                                          fnConv.funcTy,
                                          formalResultType, allResults,
                                          directResults, indirectResultAddrs,
                                          cleanups);

  assert(allResults.empty());
  assert(directResults.empty());
  assert(indirectResultAddrs.empty());

  return init;
}

void SILGenFunction::emitReturnExpr(SILLocation branchLoc,
                                    Expr *ret) {
  SmallVector<SILValue, 4> directResults;

  if (F.getConventions().hasIndirectSILResults()) {
    // Indirect return of an address-only value.
    FullExpr scope(Cleanups, CleanupLocation(ret));

    // Build an initialization which recursively destructures the tuple.
    SmallVector<CleanupHandle, 4> resultCleanups;
    InitializationPtr resultInit =
      prepareIndirectResultInit(ret->getType()->getCanonicalType(),
                                directResults, resultCleanups);

    // Emit the result expression into the initialization.
    emitExprInto(ret, resultInit.get());

    // Deactivate all the cleanups for the result values.
    for (auto cleanup : resultCleanups) {
      Cleanups.forwardCleanup(cleanup);
    }
  } else {
    // SILValue return.
    FullExpr scope(Cleanups, CleanupLocation(ret));
    RValue RV = emitRValue(ret).ensurePlusOne(*this, CleanupLocation(ret));
    std::move(RV).forwardAll(*this, directResults);
  }

  Cleanups.emitBranchAndCleanups(ReturnDest, branchLoc, directResults);
}

void StmtEmitter::visitReturnStmt(ReturnStmt *S) {
  SGF.CurrentSILLoc = S;
  SILLocation Loc = S->isImplicit() ?
                      (SILLocation)ImplicitReturnLocation(S) :
                      (SILLocation)ReturnLocation(S);

  SILValue ArgV;
  if (!S->hasResult())
    // Void return.
    SGF.Cleanups.emitBranchAndCleanups(SGF.ReturnDest, Loc);
  else if (S->getResult()->getType()->isUninhabited())
    // Never return.
    SGF.emitIgnoredExpr(S->getResult());
  else
    SGF.emitReturnExpr(Loc, S->getResult());
}

void StmtEmitter::visitThrowStmt(ThrowStmt *S) {
  ManagedValue exn = SGF.emitRValueAsSingleValue(S->getSubExpr());
  SGF.emitThrow(S, exn, /* emit a call to willThrow */ true);
}

void StmtEmitter::visitYieldStmt(YieldStmt *S) {
  SGF.CurrentSILLoc = S;

  SmallVector<ArgumentSource, 4> sources;
  SmallVector<AbstractionPattern, 4> origTypes;
  for (auto yield : S->getYields()) {
    sources.emplace_back(yield);
    origTypes.emplace_back(yield->getType());
  }

  FullExpr fullExpr(SGF.Cleanups, CleanupLocation(S));

  SGF.emitYield(S, sources, origTypes, SGF.CoroutineUnwindDest);
}

void StmtEmitter::visitPoundAssertStmt(PoundAssertStmt *stmt) {
  SILValue condition;
  {
    FullExpr scope(SGF.Cleanups, CleanupLocation(stmt));
    condition =
        SGF.emitRValueAsSingleValue(stmt->getCondition()).getUnmanagedValue();
  }

  // Extract the i1 from the Bool struct.
  auto i1Value = SGF.emitUnwrapIntegerResult(stmt, condition);

  SILValue message = SGF.B.createStringLiteral(
      stmt, stmt->getMessage(), StringLiteralInst::Encoding::UTF8);

  auto resultType = SGF.getASTContext().TheEmptyTupleType;
  SGF.B.createBuiltin(
      stmt, SGF.getASTContext().getIdentifier("poundAssert"),
      SGF.getLoweredType(resultType), {}, {i1Value, message});
}

namespace {
  // This is a little cleanup that ensures that there are no jumps out of a
  // defer body.  The cleanup is only active and installed when emitting the
  // body of a defer, and it is disabled at the end.  If it ever needs to be
  // emitted, it crashes the compiler because Sema missed something.
  class DeferEscapeCheckerCleanup : public Cleanup {
    SourceLoc deferLoc;
  public:
    DeferEscapeCheckerCleanup(SourceLoc deferLoc) : deferLoc(deferLoc) {}
    void emit(SILGenFunction &SGF, CleanupLocation l, ForUnwind_t forUnwind) override {
      assert(false && "Sema didn't catch exit out of a defer?");
    }
    void dump(SILGenFunction &) const override {
#ifndef NDEBUG
      llvm::errs() << "DeferEscapeCheckerCleanup\n"
                   << "State: " << getState() << "\n";
#endif
    }
  };
} // end anonymous namespace


namespace {
  class DeferCleanup : public Cleanup {
    SourceLoc deferLoc;
    Expr *call;
  public:
    DeferCleanup(SourceLoc deferLoc, Expr *call)
      : deferLoc(deferLoc), call(call) {}
    void emit(SILGenFunction &SGF, CleanupLocation l, ForUnwind_t forUnwind) override {
      SGF.Cleanups.pushCleanup<DeferEscapeCheckerCleanup>(deferLoc);
      auto TheCleanup = SGF.Cleanups.getTopCleanup();

      SGF.emitIgnoredExpr(call);
      
      if (SGF.B.hasValidInsertionPoint())
        SGF.Cleanups.setCleanupState(TheCleanup, CleanupState::Dead);
    }
    void dump(SILGenFunction &) const override {
#ifndef NDEBUG
      llvm::errs() << "DeferCleanup\n"
                   << "State: " << getState() << "\n";
#endif
    }
  };
} // end anonymous namespace


void StmtEmitter::visitDeferStmt(DeferStmt *S) {
  // Emit the closure for the defer, along with its binding.
  // If the defer is at the top-level code, insert 'mark_escape_inst'
  // to the top-level code to check initialization of any captured globals.
  FuncDecl *deferDecl = S->getTempDecl();
  auto declCtxKind = deferDecl->getDeclContext()->getContextKind();
  auto &sgm = SGF.SGM;
  if (declCtxKind == DeclContextKind::TopLevelCodeDecl && sgm.TopLevelSGF &&
      sgm.TopLevelSGF->B.hasValidInsertionPoint()) {
    sgm.emitMarkFunctionEscapeForTopLevelCodeGlobals(
        S, deferDecl->getCaptureInfo());
  }
  SGF.visitFuncDecl(deferDecl);

  // Register a cleanup to invoke the closure on any exit paths.
  SGF.Cleanups.pushCleanup<DeferCleanup>(S->getDeferLoc(), S->getCallExpr());
}

void StmtEmitter::visitIfStmt(IfStmt *S) {
  Scope condBufferScope(SGF.Cleanups, S);
  
  // Create a continuation block.
  JumpDest contDest = createJumpDest(S->getThenStmt());
  auto contBB = contDest.getBlock();

  // Set the destinations for any 'break' and 'continue' statements inside the
  // body.  Note that "continue" is not valid out of a labeled 'if'.
  SGF.BreakContinueDestStack.push_back(
                               { S, contDest, JumpDest(CleanupLocation(S)) });

  // Set up the block for the false case.  If there is an 'else' block, we make
  // a new one, otherwise it is our continue block.
  JumpDest falseDest = contDest;
  if (S->getElseStmt())
    falseDest = createJumpDest(S);

  // Emit the condition, along with the "then" part of the if properly guarded
  // by the condition and a jump to ContBB.  If the condition fails, jump to
  // the CondFalseBB.
  {
    // Enter a scope for any bound pattern variables.
    LexicalScope trueScope(SGF, S);

    auto NumTrueTaken = SGF.loadProfilerCount(S->getThenStmt());
    auto NumFalseTaken = SGF.loadProfilerCount(S->getElseStmt());

    SGF.emitStmtCondition(S->getCond(), falseDest, S, NumTrueTaken,
                          NumFalseTaken);

    // In the success path, emit the 'then' part if the if.
    SGF.emitProfilerIncrement(S->getThenStmt());
    SGF.emitStmt(S->getThenStmt());
  
    // Finish the "true part" by cleaning up any temporaries and jumping to the
    // continuation block.
    if (SGF.B.hasValidInsertionPoint()) {
      RegularLocation L(S->getThenStmt());
      L.pointToEnd();
      SGF.Cleanups.emitBranchAndCleanups(contDest, L);
    }
  }
  
  // If there is 'else' logic, then emit it.
  if (S->getElseStmt()) {
    SGF.B.emitBlock(falseDest.getBlock());
    visit(S->getElseStmt());
    if (SGF.B.hasValidInsertionPoint()) {
      RegularLocation L(S->getElseStmt());
      L.pointToEnd();
      SGF.B.createBranch(L, contBB);
    }
  }

  // If the continuation block was used, emit it now, otherwise remove it.
  if (contBB->pred_empty()) {
    SGF.eraseBasicBlock(contBB);
  } else {
    RegularLocation L(S->getThenStmt());
    L.pointToEnd();
    SGF.B.emitBlock(contBB, L);
  }
  SGF.BreakContinueDestStack.pop_back();
}

void StmtEmitter::visitGuardStmt(GuardStmt *S) {
  // Create a block for the body and emit code into it before processing any of
  // the patterns, because none of the bound variables will be in scope in the
  // 'body' context.
  JumpDest bodyBB =
    JumpDest(createBasicBlock(), SGF.getCleanupsDepth(), CleanupLocation(S));

  {
    // Move the insertion point to the 'body' block temporarily and emit it.
    // Note that we don't push break/continue locations since they aren't valid
    // in this statement.
    SILGenSavedInsertionPoint savedIP(SGF, bodyBB.getBlock());
    SGF.emitProfilerIncrement(S->getBody());
    SGF.emitStmt(S->getBody());

    // The body block must end in a noreturn call, return, break etc.  It
    // isn't valid to fall off into the normal flow.  To model this, we emit
    // an unreachable instruction and then have SIL diagnostic check this.
    if (SGF.B.hasValidInsertionPoint())
      SGF.B.createUnreachable(S);
  }

  // Emit the condition bindings, branching to the bodyBB if they fail.
  auto NumFalseTaken = SGF.loadProfilerCount(S->getBody());
  auto NumNonTaken = SGF.loadProfilerCount(S);
  // Begin a new 'guard' scope, which is popped when the next innermost debug
  // scope ends.
  SGF.enterDebugScope(S, /*isGuardScope=*/true);
  SGF.emitStmtCondition(S->getCond(), bodyBB, S, NumNonTaken, NumFalseTaken);
}

void StmtEmitter::visitWhileStmt(WhileStmt *S) {
  LexicalScope condBufferScope(SGF, S);

  // Create a new basic block and jump into it.
  JumpDest loopDest = createJumpDest(S->getBody());
  SGF.B.emitBlock(loopDest.getBlock(), S);
  
  // Create a break target (at this level in the cleanup stack) in case it is
  // needed.
  JumpDest breakDest = createJumpDest(S->getBody());

  // Set the destinations for any 'break' and 'continue' statements inside the
  // body.
  SGF.BreakContinueDestStack.push_back({S, breakDest, loopDest});
  
  // Evaluate the condition, the body, and a branch back to LoopBB when the
  // condition is true.  On failure, jump to BreakBB.
  {
    // Enter a scope for any bound pattern variables.
    Scope conditionScope(SGF.Cleanups, S);

    auto NumTrueTaken = SGF.loadProfilerCount(S->getBody());
    auto NumFalseTaken = SGF.loadProfilerCount(S);
    SGF.emitStmtCondition(S->getCond(), breakDest, S, NumTrueTaken, NumFalseTaken);
    
    // In the success path, emit the body of the while.
    SGF.emitProfilerIncrement(S->getBody());
    SGF.emitStmt(S->getBody());
    
    // Finish the "true part" by cleaning up any temporaries and jumping to the
    // continuation block.
    if (SGF.B.hasValidInsertionPoint()) {
      RegularLocation L(S->getBody());
      L.pointToEnd();
      SGF.Cleanups.emitBranchAndCleanups(loopDest, L);
    }
  }

  SGF.BreakContinueDestStack.pop_back();

  // Handle break block.  If it was used, we link it up with the cleanup chain,
  // otherwise we just remove it.
  SILBasicBlock *breakBB = breakDest.getBlock();
  if (breakBB->pred_empty()) {
    SGF.eraseBasicBlock(breakBB);
  } else {
    SGF.B.emitBlock(breakBB);
  }
}

void StmtEmitter::visitDoStmt(DoStmt *S) {
  // We don't need to do anything fancy if we don't have a label.
  // Otherwise, assume we might break or continue.
  bool hasLabel = (bool) S->getLabelInfo();

  JumpDest endDest = JumpDest::invalid();
  if (hasLabel) {
    // Create the end dest first so that the loop dest comes in-between.
    endDest = createJumpDest(S->getBody());

    // Create a new basic block and jump into it.
    JumpDest loopDest = createJumpDest(S->getBody());
    SGF.B.emitBlock(loopDest.getBlock(), S);

    // Set the destinations for 'break' and 'continue'.
    SGF.BreakContinueDestStack.push_back({S, endDest, loopDest});
  }

  // Emit the body.
  visit(S->getBody());

  if (hasLabel) {
    SGF.BreakContinueDestStack.pop_back();
    emitOrDeleteBlock(SGF, endDest, CleanupLocation(S));
  }
}

void StmtEmitter::visitDoCatchStmt(DoCatchStmt *S) {
  Type formalExnType = S->getCatches()
                           .front()
                           ->getCaseLabelItems()
                           .front()
                           .getPattern()
                           ->getType();
  auto &exnTL = SGF.getTypeLowering(formalExnType);

  // Create the throw destination at the end of the function.
  JumpDest throwDest = createJumpDest(S->getBody(),
                                      FunctionSection::Postmatter);
  SILArgument *exnArg = throwDest.getBlock()->createPhiArgument(
      exnTL.getLoweredType(), OwnershipKind::Owned);

  // We always need a continuation block because we might fall out of
  // a catch block.  But we don't need a loop block unless the 'do'
  // statement is labeled.
  JumpDest endDest = createJumpDest(S->getBody());

  // We don't need to do anything too fancy about emission if we don't
  // have a label.  Otherwise, assume we might break or continue.
  bool hasLabel = (bool) S->getLabelInfo();
  if (hasLabel) {
    // Create a new basic block and jump into it.
    JumpDest loopDest = createJumpDest(S->getBody());
    SGF.B.emitBlock(loopDest.getBlock(), S);

    // Set the destinations for 'break' and 'continue'.
    SGF.BreakContinueDestStack.push_back({S, endDest, loopDest});
  }

  // Emit the body.
  {
    // Push the new throw destination.
    llvm::SaveAndRestore<JumpDest> savedThrowDest(SGF.ThrowDest, throwDest);

    visit(S->getBody());
  }

  // Emit the catch clauses, but only if the body of the function
  // actually throws. This is a consequence of the fact that a
  // DoCatchStmt with a non-throwing body will type check even in
  // a non-throwing lexical context. In this case, our local throwDest
  // has no predecessors, and SGF.ThrowDest may not be valid either.
  if (auto *BB = getOrEraseBlock(SGF, throwDest)) {
    // Move the insertion point to the throw destination.
    SILGenSavedInsertionPoint savedIP(SGF, BB, FunctionSection::Postmatter);

    // The exception cleanup should be getting forwarded around
    // correctly anyway, but push a scope to ensure it gets popped.
    Scope exnScope(SGF.Cleanups, CleanupLocation(S));

    // Take ownership of the exception.
    ManagedValue exn = SGF.emitManagedRValueWithCleanup(exnArg, exnTL);

    // Emit all the catch clauses, branching to the end destination if
    // we fall out of one.
    SGF.emitCatchDispatch(S, exn, S->getCatches(), endDest);

    // We assume that exn's cleanup is still valid at this point. To ensure that
    // we do not re-emit it and do a double consume, we rely on us having
    // finished emitting code and thus unsetting the insertion point here. This
    // assert is to make sure this invariant is clear in the code and validated.
    assert(!SGF.B.hasValidInsertionPoint());
  }

  if (hasLabel) {
    SGF.BreakContinueDestStack.pop_back();
  }

  // Handle falling out of the do-block.
  //
  // It's important for good code layout that the insertion point be
  // left in the original function section after this.  So if
  // emitOrDeleteBlock ever learns to just continue in the
  // predecessor, we'll need to suppress that here.
  emitOrDeleteBlock(SGF, endDest, CleanupLocation(S->getBody()));
}

void StmtEmitter::visitRepeatWhileStmt(RepeatWhileStmt *S) {
  // Create a new basic block and jump into it.
  SILBasicBlock *loopBB = createBasicBlock();
  SGF.B.emitBlock(loopBB, S);
  
  // Set the destinations for 'break' and 'continue'
  JumpDest endDest = createJumpDest(S->getBody());
  JumpDest condDest = createJumpDest(S->getBody());
  SGF.BreakContinueDestStack.push_back({ S, endDest, condDest });

  // Emit the body, which is always evaluated the first time around.
  SGF.emitProfilerIncrement(S->getBody());
  visit(S->getBody());

  // Let's not differ from C99 6.8.5.2: "The evaluation of the controlling
  // expression takes place after each execution of the loop body."
  emitOrDeleteBlock(SGF, condDest, S);

  if (SGF.B.hasValidInsertionPoint()) {
    // Evaluate the condition with the false edge leading directly
    // to the continuation block.
    auto NumTrueTaken = SGF.loadProfilerCount(S->getBody());
    auto NumFalseTaken = SGF.loadProfilerCount(S);
    Condition Cond = SGF.emitCondition(S->getCond(),
                                       /*invertValue*/ false, /*contArgs*/ {},
                                       NumTrueTaken, NumFalseTaken);

    Cond.enterTrue(SGF);
    if (SGF.B.hasValidInsertionPoint()) {
      SGF.B.createBranch(S->getCond(), loopBB);
    }
    
    Cond.exitTrue(SGF);
    // Complete the conditional execution.
    Cond.complete(SGF);
  }
  
  emitOrDeleteBlock(SGF, endDest, S);
  SGF.BreakContinueDestStack.pop_back();
}

void StmtEmitter::visitAsyncForEachStmt(ForEachStmt *S) {

  // Dig out information about the sequence conformance.
  auto sequenceConformance = S->getSequenceConformance();
  Type sequenceType = S->getSequence()->getType();
  
  auto asyncSequenceProto =
      SGF.getASTContext().getProtocol(KnownProtocolKind::AsyncSequence);
  auto sequenceSubs = SubstitutionMap::getProtocolSubstitutions(
      asyncSequenceProto, sequenceType, sequenceConformance);

  // Emit the 'generator' variable that we'll be using for iteration.
  LexicalScope OuterForScope(SGF, CleanupLocation(S));
  {
    auto initialization =
        SGF.emitInitializationForVarDecl(S->getIteratorVar(), false);
    SILLocation loc = SILLocation(S->getSequence());

    // Compute the reference to the AsyncSequence's makeAsyncSequence().
    FuncDecl *makeGeneratorReq = 
      SGF.getASTContext().getAsyncSequenceMakeAsyncIterator();
    ConcreteDeclRef makeGeneratorRef(makeGeneratorReq, sequenceSubs);

    // Call makeAsyncSequence().
    RValue result = SGF.emitApplyMethod(
        loc, makeGeneratorRef, ArgumentSource(S->getSequence()),
        PreparedArguments(ArrayRef<AnyFunctionType::Param>({})),
        SGFContext(initialization.get()));
    if (!result.isInContext()) {
      ArgumentSource(SILLocation(S->getSequence()),
                     std::move(result).ensurePlusOne(SGF, loc))
          .forwardInto(SGF, initialization.get());
    }
  }

  // If we ever reach an unreachable point, stop emitting statements.
  // This will need revision if we ever add goto.
  if (!SGF.B.hasValidInsertionPoint()) return;
  
  // If generator's optional result is address-only, create a stack allocation
  // to hold the results.  This will be initialized on every entry into the loop
  // header and consumed by the loop body. On loop exit, the terminating value
  // will be in the buffer.
  CanType optTy;
  if (S->getConvertElementExpr()) {
    optTy = S->getConvertElementExpr()->getType()->getCanonicalType();
  } else {
    optTy = OptionalType::get(S->getSequenceConformance().getTypeWitnessByName(
                                  S->getSequence()->getType(),
                                  SGF.getASTContext().Id_Element))
                ->getCanonicalType();
  }
  auto &optTL = SGF.getTypeLowering(optTy);
  SILValue addrOnlyBuf;
  ManagedValue nextBufOrValue;

  if (optTL.isAddressOnly() && SGF.silConv.useLoweredAddresses())
    addrOnlyBuf = SGF.emitTemporaryAllocation(S, optTL.getLoweredType());

  // Create a new basic block and jump into it.
  JumpDest loopDest = createJumpDest(S->getBody());
  SGF.B.emitBlock(loopDest.getBlock(), S);

  // Compute the reference to the the generator's next() && cancel().
  auto generatorProto =
      SGF.getASTContext().getProtocol(KnownProtocolKind::AsyncIteratorProtocol);
  ValueDecl *generatorNextReq = generatorProto->getSingleRequirement(
      DeclName(SGF.getASTContext(), SGF.getASTContext().Id_next,
               ArrayRef<Identifier>()));
  auto generatorAssocType =
      asyncSequenceProto->getAssociatedType(SGF.getASTContext().Id_AsyncIterator);
  auto generatorMemberRef = DependentMemberType::get(
      asyncSequenceProto->getSelfInterfaceType(), generatorAssocType);
  auto generatorType = sequenceConformance.getAssociatedType(
      sequenceType, generatorMemberRef);
  auto generatorConformance = sequenceConformance.getAssociatedConformance(
      sequenceType, generatorMemberRef, generatorProto);
  auto generatorSubs = SubstitutionMap::getProtocolSubstitutions(
      generatorProto, generatorType, generatorConformance);
  ConcreteDeclRef generatorNextRef(generatorNextReq, generatorSubs);

  // Set the destinations for 'break' and 'continue'.
  JumpDest endDest = createJumpDest(S->getBody());
  SGF.BreakContinueDestStack.push_back({ S, endDest, loopDest });
  

  auto buildArgumentSource = [&]() {
    if (cast<FuncDecl>(generatorNextRef.getDecl())->getSelfAccessKind() ==
        SelfAccessKind::Mutating) {
      LValue lv =
          SGF.emitLValue(S->getIteratorVarRef(), SGFAccessKind::ReadWrite);
      return ArgumentSource(S, std::move(lv));
    }
    LValue lv =
        SGF.emitLValue(S->getIteratorVarRef(), SGFAccessKind::OwnedObjectRead);
    return ArgumentSource(
        S, SGF.emitLoadOfLValue(S->getIteratorVarRef(), std::move(lv),
                                SGFContext().withFollowingSideEffects()));
  };

  auto buildElementRValue = [&](SILLocation loc, SGFContext ctx) {
    RValue result;
    result = SGF.emitApplyMethod(
        loc, generatorNextRef, buildArgumentSource(),
        PreparedArguments(ArrayRef<AnyFunctionType::Param>({})),
        S->getElementExpr() ? SGFContext() : ctx);
    if (S->getElementExpr()) {
      SILGenFunction::OpaqueValueRAII pushOpaqueValue(
          SGF, S->getElementExpr(),
          std::move(result).getAsSingleValue(SGF, loc));
      result = SGF.emitRValue(S->getConvertElementExpr(), ctx);
    }
    return result;
  };

  // Then emit the loop destination block.
  //
  // Advance the generator.  Use a scope to ensure that any temporary stack
  // allocations in the subexpression are immediately released.
  if (optTL.isAddressOnly() && SGF.silConv.useLoweredAddresses()) {
    // Create the initialization outside of the innerForScope so that the
    // innerForScope doesn't clean it up.
    auto nextInit = SGF.useBufferAsTemporary(addrOnlyBuf, optTL);
    {
      ArgumentScope innerForScope(SGF, SILLocation(S));
      SILLocation loc = SILLocation(S);
      RValue result = buildElementRValue(loc, SGFContext(nextInit.get()));
      if (!result.isInContext()) {
        ArgumentSource(SILLocation(S->getSequence()),
                       std::move(result).ensurePlusOne(SGF, loc))
            .forwardInto(SGF, nextInit.get());
      }
      innerForScope.pop();
    }
    nextBufOrValue = nextInit->getManagedAddress();
  } else {
    ArgumentScope innerForScope(SGF, SILLocation(S));
    nextBufOrValue = innerForScope.popPreservingValue(
        buildElementRValue(SILLocation(S), SGFContext())
            .getAsSingleValue(SGF, SILLocation(S)));
  }

  SILBasicBlock *failExitingBlock = createBasicBlock();
  SwitchEnumBuilder switchEnumBuilder(SGF.B, S, nextBufOrValue);

  switchEnumBuilder.addOptionalSomeCase(
      createBasicBlock(), loopDest.getBlock(),
      [&](ManagedValue inputValue, SwitchCaseFullExpr &&scope) {
        SGF.emitProfilerIncrement(S->getBody());

        // Emit the loop body.
        // The declared variable(s) for the current element are destroyed
        // at the end of each loop iteration.
        {
          Scope innerForScope(SGF.Cleanups, CleanupLocation(S->getBody()));
          // Emit the initialization for the pattern.  If any of the bound
          // patterns
          // fail (because this is a 'for case' pattern with a refutable
          // pattern,
          // the code should jump to the continue block.
          InitializationPtr initLoopVars =
              SGF.emitPatternBindingInitialization(S->getPattern(), loopDest);

          // If we had a loadable "next" generator value, we know it is present.
          // Get the value out of the optional, and wrap it up with a cleanup so
          // that any exits out of this scope properly clean it up.
          //
          // *NOTE* If we do not have an address only value, then inputValue is
          // *already properly unwrapped.
          if (optTL.isAddressOnly() && SGF.silConv.useLoweredAddresses()) {
            inputValue = SGF.emitUncheckedGetOptionalValueFrom(
                S, inputValue, optTL, SGFContext(initLoopVars.get()));
          }

          if (!inputValue.isInContext())
            RValue(SGF, S, optTy.getOptionalObjectType(), inputValue)
                .forwardInto(SGF, S, initLoopVars.get());

          // Now that the pattern has been initialized, check any where
          // condition.
          // If it fails, loop around as if 'continue' happened.
          if (auto *Where = S->getWhere()) {
            auto cond = SGF.emitCondition(Where, /*invert*/ true);
            // If self is null, branch to the epilog.
            cond.enterTrue(SGF);
            SGF.Cleanups.emitBranchAndCleanups(loopDest, Where, {});
            cond.exitTrue(SGF);
            cond.complete(SGF);
          }

          visit(S->getBody());
        }

        // If we emitted an unreachable in the body, we will not have a valid
        // insertion point. Just return early.
        if (!SGF.B.hasValidInsertionPoint()) {
          scope.unreachableExit();
          return;
        }

        // Otherwise, associate the loop body's closing brace with this branch.
        RegularLocation L(S->getBody());
        L.pointToEnd();
        scope.exitAndBranch(L);
      },
      SGF.loadProfilerCount(S->getBody()));

  // We add loop fail block, just to be defensive about intermediate
  // transformations performing cleanups at scope.exit(). We still jump to the
  // contBlock.
  switchEnumBuilder.addOptionalNoneCase(
      createBasicBlock(), failExitingBlock,
      [&](ManagedValue inputValue, SwitchCaseFullExpr &&scope) {
        assert(!inputValue && "None should not be passed an argument!");
        scope.exitAndBranch(S);
      },
      SGF.loadProfilerCount(S));

  std::move(switchEnumBuilder).emit();

  SGF.B.emitBlock(failExitingBlock);
  emitOrDeleteBlock(SGF, endDest, S);
  SGF.BreakContinueDestStack.pop_back();
}

void StmtEmitter::visitForEachStmt(ForEachStmt *S) {
  if (S->getAwaitLoc().isValid()) {
    visitAsyncForEachStmt(S);
    return;
  }

  // Dig out information about the sequence conformance.
  auto sequenceConformance = S->getSequenceConformance();
  Type sequenceType = S->getSequence()->getType();
  
  auto sequenceProto =
      SGF.getASTContext().getProtocol(KnownProtocolKind::Sequence);
  auto sequenceSubs = SubstitutionMap::getProtocolSubstitutions(
      sequenceProto, sequenceType, sequenceConformance);

  // Emit the 'iterator' variable that we'll be using for iteration.
  LexicalScope OuterForScope(SGF, CleanupLocation(S));
  {
    auto initialization =
        SGF.emitInitializationForVarDecl(S->getIteratorVar(), false);
    SILLocation loc = SILLocation(S->getSequence());

    // Compute the reference to the Sequence's makeIterator().
    FuncDecl *makeIteratorReq = SGF.getASTContext().getSequenceMakeIterator();
    ConcreteDeclRef makeIteratorRef(makeIteratorReq, sequenceSubs);

    // Call makeIterator().
    RValue result = SGF.emitApplyMethod(
        loc, makeIteratorRef, ArgumentSource(S->getSequence()),
        PreparedArguments(ArrayRef<AnyFunctionType::Param>({})),
        SGFContext(initialization.get()));
    if (!result.isInContext()) {
      ArgumentSource(SILLocation(S->getSequence()),
                     std::move(result).ensurePlusOne(SGF, loc))
          .forwardInto(SGF, initialization.get());
    }
  }

  // If we ever reach an unreachable point, stop emitting statements.
  // This will need revision if we ever add goto.
  if (!SGF.B.hasValidInsertionPoint()) return;
  
  // If generator's optional result is address-only, create a stack allocation
  // to hold the results.  This will be initialized on every entry into the loop
  // header and consumed by the loop body. On loop exit, the terminating value
  // will be in the buffer.
  CanType optTy =
      OptionalType::get(
          S->getSequenceConformance().getTypeWitnessByName(
              S->getSequence()->getType(), SGF.getASTContext().Id_Element))
          ->getCanonicalType();
  auto &optTL = SGF.getTypeLowering(optTy);

  SILValue addrOnlyBuf;
  bool nextResultTyIsAddressOnly =
      optTL.isAddressOnly() && SGF.silConv.useLoweredAddresses();

  if (nextResultTyIsAddressOnly)
    addrOnlyBuf = SGF.emitTemporaryAllocation(S, optTL.getLoweredType());

  // Create a new basic block and jump into it.
  JumpDest loopDest = createJumpDest(S->getBody());
  SGF.B.emitBlock(loopDest.getBlock(), S);

  // Set the destinations for 'break' and 'continue'.
  JumpDest endDest = createJumpDest(S->getBody());
  SGF.BreakContinueDestStack.push_back({ S, endDest, loopDest });

  // Compute the reference to the the iterator's next().
  auto iteratorProto =
      SGF.getASTContext().getProtocol(KnownProtocolKind::IteratorProtocol);
  ValueDecl *iteratorNextReq = iteratorProto->getSingleRequirement(
      DeclName(SGF.getASTContext(), SGF.getASTContext().Id_next,
               ArrayRef<Identifier>()));
  auto iteratorAssocType =
      sequenceProto->getAssociatedType(SGF.getASTContext().Id_Iterator);
  auto iteratorMemberRef = DependentMemberType::get(
      sequenceProto->getSelfInterfaceType(), iteratorAssocType);
  auto iteratorType = sequenceConformance.getAssociatedType(
      sequenceType, iteratorMemberRef);
  auto iteratorConformance = sequenceConformance.getAssociatedConformance(
      sequenceType, iteratorMemberRef, iteratorProto);
  auto iteratorSubs = SubstitutionMap::getProtocolSubstitutions(
      iteratorProto, iteratorType, iteratorConformance);
  ConcreteDeclRef iteratorNextRef(iteratorNextReq, iteratorSubs);

  auto buildArgumentSource = [&]() {
    if (cast<FuncDecl>(iteratorNextRef.getDecl())->getSelfAccessKind() ==
        SelfAccessKind::Mutating) {
      LValue lv =
          SGF.emitLValue(S->getIteratorVarRef(), SGFAccessKind::ReadWrite);
      return ArgumentSource(S, std::move(lv));
    }
    LValue lv =
        SGF.emitLValue(S->getIteratorVarRef(), SGFAccessKind::OwnedObjectRead);
    return ArgumentSource(
        S, SGF.emitLoadOfLValue(S->getIteratorVarRef(), std::move(lv),
                                SGFContext().withFollowingSideEffects()));
  };

  bool hasElementConversion = S->getElementExpr();
  auto buildElementRValue = [&](SILLocation loc, SGFContext ctx) {
    RValue result;
    result = SGF.emitApplyMethod(
        loc, iteratorNextRef, buildArgumentSource(),
        PreparedArguments(ArrayRef<AnyFunctionType::Param>({})),
        hasElementConversion ? SGFContext() : ctx);
    return result;
  };

  ManagedValue nextBufOrElement;
  // Then emit the loop destination block.
  //
  // Advance the generator.  Use a scope to ensure that any temporary stack
  // allocations in the subexpression are immediately released.
  if (nextResultTyIsAddressOnly) {
    // Create the initialization outside of the innerForScope so that the
    // innerForScope doesn't clean it up.
    auto nextInit = SGF.useBufferAsTemporary(addrOnlyBuf, optTL);
    {
      ArgumentScope innerForScope(SGF, SILLocation(S));
      SILLocation loc = SILLocation(S);
      RValue result = buildElementRValue(loc, SGFContext(nextInit.get()));
      if (!result.isInContext()) {
        ArgumentSource(SILLocation(S->getSequence()),
                       std::move(result).ensurePlusOne(SGF, loc))
            .forwardInto(SGF, nextInit.get());
      }
      innerForScope.pop();
    }
    nextBufOrElement = nextInit->getManagedAddress();
  } else {
    ArgumentScope innerForScope(SGF, SILLocation(S));
    nextBufOrElement = innerForScope.popPreservingValue(
        buildElementRValue(SILLocation(S), SGFContext())
            .getAsSingleValue(SGF, SILLocation(S)));
  }

  SILBasicBlock *failExitingBlock = createBasicBlock();
  SwitchEnumBuilder switchEnumBuilder(SGF.B, S, nextBufOrElement);

  auto convertElementRValue = [&](ManagedValue inputValue, SGFContext ctx) -> ManagedValue {
    SILGenFunction::OpaqueValueRAII pushOpaqueValue(SGF, S->getElementExpr(),
                                                    inputValue);
    return SGF.emitRValue(S->getConvertElementExpr(), ctx)
        .getAsSingleValue(SGF, SILLocation(S));
  };

  switchEnumBuilder.addOptionalSomeCase(
      createBasicBlock(), loopDest.getBlock(),
      [&](ManagedValue inputValue, SwitchCaseFullExpr &&scope) {
        SGF.emitProfilerIncrement(S->getBody());

        // Emit the loop body.
        // The declared variable(s) for the current element are destroyed
        // at the end of each loop iteration.
        {
          Scope innerForScope(SGF.Cleanups, CleanupLocation(S->getBody()));
          // Emit the initialization for the pattern.  If any of the bound
          // patterns
          // fail (because this is a 'for case' pattern with a refutable
          // pattern,
          // the code should jump to the continue block.
          InitializationPtr initLoopVars =
              SGF.emitPatternBindingInitialization(S->getPattern(), loopDest);

          // If we had a loadable "next" generator value, we know it is present.
          // Get the value out of the optional, and wrap it up with a cleanup so
          // that any exits out of this scope properly clean it up.
          //
          // *NOTE* If we do not have an address only value, then inputValue is
          // *already properly unwrapped.
          SGFContext loopVarCtx{initLoopVars.get()};
          if (nextResultTyIsAddressOnly) {
            inputValue = SGF.emitUncheckedGetOptionalValueFrom(
                S, inputValue, optTL,
                hasElementConversion ? SGFContext() : loopVarCtx);
          }

          CanType optConvertedTy = optTy;
          if (hasElementConversion) {
            inputValue = convertElementRValue(inputValue, loopVarCtx);
            optConvertedTy =
                OptionalType::get(S->getConvertElementExpr()->getType())
                    ->getCanonicalType();
          }
          if (!inputValue.isInContext())
            RValue(SGF, S, optConvertedTy.getOptionalObjectType(), inputValue)
                .forwardInto(SGF, S, initLoopVars.get());

          // Now that the pattern has been initialized, check any where
          // condition.
          // If it fails, loop around as if 'continue' happened.
          if (auto *Where = S->getWhere()) {
            auto cond = SGF.emitCondition(Where, /*invert*/ true);
            // If self is null, branch to the epilog.
            cond.enterTrue(SGF);
            SGF.Cleanups.emitBranchAndCleanups(loopDest, Where, {});
            cond.exitTrue(SGF);
            cond.complete(SGF);
          }

          visit(S->getBody());
        }

        // If we emitted an unreachable in the body, we will not have a valid
        // insertion point. Just return early.
        if (!SGF.B.hasValidInsertionPoint()) {
          scope.unreachableExit();
          return;
        }

        // Otherwise, associate the loop body's closing brace with this branch.
        RegularLocation L(S->getBody());
        L.pointToEnd();
        scope.exitAndBranch(L);
      },
      SGF.loadProfilerCount(S->getBody()));

  // We add loop fail block, just to be defensive about intermediate
  // transformations performing cleanups at scope.exit(). We still jump to the
  // contBlock.
  switchEnumBuilder.addOptionalNoneCase(
      createBasicBlock(), failExitingBlock,
      [&](ManagedValue inputValue, SwitchCaseFullExpr &&scope) {
        assert(!inputValue && "None should not be passed an argument!");
        scope.exitAndBranch(S);
      },
      SGF.loadProfilerCount(S));

  std::move(switchEnumBuilder).emit();

  SGF.B.emitBlock(failExitingBlock);
  emitOrDeleteBlock(SGF, endDest, S);
  SGF.BreakContinueDestStack.pop_back();
}

void StmtEmitter::visitBreakStmt(BreakStmt *S) {
  assert(S->getTarget() && "Sema didn't fill in break target?");
  SGF.emitBreakOutOf(S, S->getTarget());
}

void SILGenFunction::emitBreakOutOf(SILLocation loc, Stmt *target) {
  CurrentSILLoc = loc;
  
  // Find the target JumpDest based on the target that sema filled into the
  // stmt.
  for (auto &elt : BreakContinueDestStack) {
    if (target == elt.Target) {
      Cleanups.emitBranchAndCleanups(elt.BreakDest, loc);
      return;
    }
  }
  llvm_unreachable("Break has available target block.");
}

void StmtEmitter::visitContinueStmt(ContinueStmt *S) {
  assert(S->getTarget() && "Sema didn't fill in continue target?");

  SGF.CurrentSILLoc = S;
  
  // Find the target JumpDest based on the target that sema filled into the
  // stmt.
  for (auto &elt : SGF.BreakContinueDestStack) {
    if (S->getTarget() == elt.Target) {
      SGF.Cleanups.emitBranchAndCleanups(elt.ContinueDest, S);
      return;
    }
  }
  llvm_unreachable("Continue has available target block.");
}

void StmtEmitter::visitSwitchStmt(SwitchStmt *S) {
  // Implemented in SILGenPattern.cpp.
  SGF.emitSwitchStmt(S);
}

void StmtEmitter::visitCaseStmt(CaseStmt *S) {
  llvm_unreachable("cases should be lowered as part of switch stmt");
}

void StmtEmitter::visitFallthroughStmt(FallthroughStmt *S) {
  // Implemented in SILGenPattern.cpp.
  SGF.emitSwitchFallthrough(S);
}

void StmtEmitter::visitFailStmt(FailStmt *S) {
  // Jump to the failure block.
  assert(SGF.FailDest.isValid() && "too big to fail");
  SGF.Cleanups.emitBranchAndCleanups(SGF.FailDest, S);
}

/// Return a basic block suitable to be the destination block of a
/// try_apply instruction.  The block is implicitly emitted and filled in.
SILBasicBlock *
SILGenFunction::getTryApplyErrorDest(SILLocation loc,
                                     CanSILFunctionType fnTy,
                                     ExecutorBreadcrumb prevExecutor,
                                     SILResultInfo exnResult,
                                     bool suppressErrorPath) {
  assert(exnResult.getConvention() == ResultConvention::Owned);

  // For now, don't try to re-use destination blocks for multiple
  // failure sites.
  SILBasicBlock *destBB = createBasicBlock(FunctionSection::Postmatter);
  SILValue exn = destBB->createPhiArgument(getSILType(exnResult, fnTy),
                                           OwnershipKind::Owned);

  assert(B.hasValidInsertionPoint() && B.insertingAtEndOfBlock());
  SILGenSavedInsertionPoint savedIP(*this, destBB, FunctionSection::Postmatter);

  prevExecutor.emit(*this, loc);

  // If we're suppressing error paths, just wrap it up as unreachable
  // and return.
  if (suppressErrorPath) {
    B.createUnreachable(loc);
    return destBB;
  }

  // We don't want to exit here with a dead cleanup on the stack,
  // so push the scope first.
  FullExpr scope(Cleanups, CleanupLocation(loc));
  emitThrow(loc, emitManagedRValueWithCleanup(exn));

  return destBB;
}

void SILGenFunction::emitThrow(SILLocation loc, ManagedValue exnMV,
                               bool emitWillThrow) {
  assert(ThrowDest.isValid() &&
         "calling emitThrow with invalid throw destination!");

  // Claim the exception value.  If we need to handle throwing
  // cleanups, the correct thing to do here is to recreate the
  // exception's cleanup when emitting each cleanup we branch through.
  // But for now we aren't bothering.
  SILValue exn = exnMV.forward(*this);

  if (emitWillThrow) {
    // Generate a call to the 'swift_willThrow' runtime function to allow the
    // debugger to catch the throw event.
    B.createBuiltin(loc, SGM.getASTContext().getIdentifier("willThrow"),
                    SGM.Types.getEmptyTupleType(), {}, {exn});
  }

  // Branch to the cleanup destination.
  Cleanups.emitBranchAndCleanups(ThrowDest, loc, exn, IsForUnwind);
}
