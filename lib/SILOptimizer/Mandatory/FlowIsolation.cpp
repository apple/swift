//===-- FlowIsolation.cpp - Enforces flow-sensitive actor isolation rules -===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "flow-isolation"

#include "llvm/Support/WithColor.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ActorIsolation.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BitDataflow.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

namespace {

class AnalysisInfo;

// MARK: utilities

static SILFunction* getCallee(SILInstruction *someInst) {
  if (auto apply = ApplySite::isa(someInst))
    if (SILFunction *callee = apply.getCalleeFunction())
      return callee;
  return nullptr;
}

// Represents the state of isolation for `self` during the flow-analysis,
// at entry and exit to a block. The states are part of a semi-lattice,
// where the extra top element represents a conflict in isolation:
//
//         T = "top"
//        / \
//      Iso  NonIso
//        \ /
//         B = "bottom"
//
// While we will be talking about isolated vs nonisolated uses, the only
// isolated uses that we consider are stored property accesses.
struct State {
  // Each state kind, as an integer, is its position in any bit vectors.
  enum Kind {
    Isolated = 0,
    Nonisolated = 1
  };

  // Number of states, excluding Top or Bottom, in this flow problem.
  static constexpr unsigned NumStates = 2;
};

/// Information gathered for analysis that is specific to a block.
struct Info {
  using UseSet = SmallPtrSet<SILInstruction*, 8>;

  /// Records all nonisolated uses of `self` in the block, and their kind of
  /// use to aid diagnostics.
  UseSet nonisolatedUses;

  /// Records all stored property uses based on `self` in the block.
  /// These are the only isolated uses that we care about.
  UseSet propertyUses;

  Info() : nonisolatedUses(), propertyUses() {}

  // Diagnoses all property uses as being an error.
  void diagnoseAll(AnalysisInfo &info, bool forDeinit,
                   SILInstruction *blame = nullptr);

  /// Returns the block corresponding to this information.
  SILBasicBlock* getBlock() const {
    if (!propertyUses.empty())
      return (*(propertyUses.begin()))->getParent();

    if (!nonisolatedUses.empty())
      return (*(nonisolatedUses.begin()))->getParent();

    // I only expect to call this when there's a use, so to save memory
    // we compute the corresponding block from its stored uses.
    assert(false && "no uses to determine block");
    return nullptr;
  }

  SILInstruction* firstPropertyUse() const {
    auto *blk = getBlock();

    for (auto &inst : *blk) {
      if (propertyUses.count(&inst))
        return &inst;
    }

    assert(false && "no first property use found!");
    return nullptr;
  }

  bool hasNonisolatedUse() const {
    return !nonisolatedUses.empty();
  }

  bool hasPropertyUse() const {
    return !propertyUses.empty();
  }

  void dump() const LLVM_ATTRIBUTE_USED {
    llvm::dbgs() << "nonisolatedUses:\n";
    for (auto const *i : nonisolatedUses)
      i->dump();

    llvm::dbgs() << "propertyUses:\n";
    for (auto const *i : propertyUses)
      i->dump();
  }
};

/// \returns true iff the function is a deinit, or a defer of a deinit.
static bool isWithinDeinit(SILFunction *fn) {
  auto *astFn = fn->getDeclContext()->getAsDecl();

  if (auto *funcDecl = dyn_cast<FuncDecl>(astFn))
    if (funcDecl->isDeferBody())
      astFn = funcDecl->getParent()->getAsDecl();

  return isa<DestructorDecl>(astFn);
}

/// Carries the state of analysis for an entire SILFunction.
class AnalysisInfo : public BasicBlockData<Info> {
private:
  /// Isolation state at the start of the entry block to this function.
  /// This should always be `isolated`, unless if this is a `defer`.
  State::Kind startingIsolation = State::Isolated;

public:

  // The deferBlocks information is shared between all blocks of
  // this analysis information's function.
  llvm::SmallMapVector< SILFunction*,
                        std::unique_ptr<AnalysisInfo>, 8> deferBlocks;

  // Only computed after calling solve()
  BitDataflow flow;

  /// This value represents the outgoing isolation state of the function if
  /// a normal return is reached, along with the block that returns normally.
  /// Only computed after calling solve(), where it remains None if the function
  /// doesn't return normally.
  llvm::Optional<std::pair<SILBasicBlock *, State::Kind>> normalReturn =
      llvm::None;

  /// indicates whether the SILFunction is (or contained in) a deinit.
  bool forDeinit;

  AnalysisInfo(SILFunction *fn) : BasicBlockData<Info>(fn),
                                  flow(fn, State::NumStates) {
    forDeinit = isWithinDeinit(fn);
  }

  // analyzes the function for uses of `self`.
  void analyze(const SILArgument* selfParam);

  // Solves the data-flow problem, assuming analysis has been performed.
  void solve();

  // Verifies uses in this function, assuming solving has been performed.
  void verifyIsolation();

  /// Finds an appropriate instruction that can be blamed for introducing a
  /// source of `nonisolation` in a control-flow path leading the given
  /// instruction. Preferring the closest block. Use for diagnostics.
  /// \param start an instruction that can be reached by a `nonisolated`
  /// use in the CFG.
  /// \returns an instruction that can be used for blame in a diagnostic.
  SILInstruction *findNonisolatedBlame(SILInstruction *start);

  void diagnoseEntireFunction(SILInstruction* blame) {
    assert(blame);
    for (auto bnd : *this)
      bnd.data.diagnoseAll(*this, forDeinit, blame);
  }

  /// Does this function have a nonisolated use?
  bool hasNonisolatedUse() const {
    for (auto const& bnd : *this)
      if (bnd.data.hasNonisolatedUse())
        return true;

    return false;
  }

  /// Does this function have a property use?
  bool hasPropertyUse() const {
    for (auto const& bnd : *this)
      if (bnd.data.hasPropertyUse())
        return true;

    return false;
  }

  /// Do we have sub-analysis information for this function, as a defer body?
  bool haveDeferInfo(SILFunction *someFn) {
    assert(someFn);
    return deferBlocks.count(someFn) > 0;
  }

  AnalysisInfo& getOrCreateDeferInfo(SILFunction* someFn) {
    assert(someFn);

    if (haveDeferInfo(someFn))
      return *(deferBlocks[someFn]);

    // otherwise, insert fresh info and retry.
    deferBlocks.insert({someFn, std::make_unique<AnalysisInfo>(someFn)});
    return getOrCreateDeferInfo(someFn);
  }

  /// Records an incoming isolation kind to this function from a call-site.
  /// \returns true iff the start state has changed from isolated to nonisolated
  bool setNonisolatedStart() {
    // once we enter the nonisolated state, nothing will change that.
    if (startingIsolation == State::Nonisolated)
      return false;

    startingIsolation = State::Nonisolated;
    return true;
  }

  /// Test whether the incoming isolation kind was set to nonisolated.
  bool hasNonisolatedStart() const {
    return startingIsolation == State::Nonisolated;
  }

  /// Records that the instruction accesses an isolated property.
  void markPropertyUse(SILInstruction *i) {
    LLVM_DEBUG(llvm::dbgs() << "marking as isolated: " << *i);
    auto &blockData = this->operator[](i->getParent());
    blockData.propertyUses.insert(i);
  }

  /// Records that the instruction causes 'self' to become nonisolated.
  void markNonIsolated(SILInstruction *i) {
    LLVM_DEBUG(llvm::dbgs() << "marking as non-isolated: " << *i);
    auto &blockData = this->operator[](i->getParent());
    blockData.nonisolatedUses.insert(i);
  }

  void dump() const LLVM_ATTRIBUTE_USED {
    llvm::dbgs() << "analysis-info for " << getFunction()->getName() << "\n";
    for (auto const& bnd : *this) {
      llvm::dbgs() << "bb" << bnd.block.getDebugID() << "\n";
      bnd.data.dump();
    }
    llvm::dbgs() << "flow-problem state:\n";
    flow.dump();

    // print the defer information in a different color, if supported.
    llvm::WithColor color(llvm::dbgs(), raw_ostream::BLUE);
    for (auto const& entry : deferBlocks)
      entry.second->dump();
  }
};

// MARK: diagnostics

SILInstruction *AnalysisInfo::findNonisolatedBlame(SILInstruction* startInst) {
  assert(startInst);

  SILBasicBlock* firstBlk = startInst->getParent();
  assert(firstBlk->getParent() == getFunction());

  // searches the a block starting at the provided position in reverse
  // order of instructions (i.e., from terminator to first instruction).
  auto searchBlockForNonisolated =
      [&](SILBasicBlock::reverse_iterator cursor) -> SILInstruction * {
    SILBasicBlock *block = cursor->getParent();
    auto &state = flow[block];

    // does this block generate non-isolation?
    if (state.genSet[State::Nonisolated]) {
      auto &data = this->operator[](block);
      assert(!data.nonisolatedUses.empty());

      // scan from the cursor backwards in this block.
      while (cursor != block->rend()) {
        auto *inst = &*cursor;
        cursor++;

        if (data.nonisolatedUses.count(inst)) {
          return inst;
        }
      }
    }

    return nullptr;
  };

  // whether we should visit a given predecessor block in the search.
  auto shouldVisit = [&](SILBasicBlock *pred) {
    // visit blocks that contribute nonisolation to successors.
    return flow[pred].exitSet[State::Nonisolated];
  };

  // first check if the nonisolated use precedes the start instruction in
  // this same block.
  if (auto *inst = searchBlockForNonisolated(startInst->getReverseIterator()))
    return inst;

  // Seed a workQueue with the predecessors of this start block to
  // begin a breadth-first search to find one of the closest predecessors.
  BasicBlockWorkqueue workQueue(firstBlk->getFunction());
  for (auto *pred : firstBlk->getPredecessorBlocks())
    if (shouldVisit(pred))
      workQueue.push(pred);

  while (auto *block = workQueue.pop()) {
    // do we have a nonisolated use here?
    if (auto *inst = searchBlockForNonisolated(block->rbegin()))
      return inst;

    // otherwise keep looking
    for (auto *pred : block->getPredecessorBlocks()) {
      if (shouldVisit(pred))
        workQueue.pushIfNotVisited(pred);
    }
  }

  llvm_unreachable("failed to find nonisolated blame.");
}

static StringRef verbForInvoking(ValueDecl *value) {
  // Only computed properties need a different verb.
  if (isa<AbstractStorageDecl>(value))
    return "accessing ";

  return "calling ";
}

/// For a specific note diagnostic that describes the blamed instruction for
/// introducing non-isolation, this function produces the values needed
/// to describe it to the user. Thus, the implementation of this function is
/// closely tied to that diagnostic.
static std::tuple<StringRef, StringRef, DeclName>
describe(SILInstruction *blame) {
  auto &ctx = blame->getModule().getASTContext();

  // check if it's a call-like thing.
  if (auto apply = ApplySite::isa(blame)) {
    /// First, look for a callee declaration.
    ///
    /// We can't use ApplySite::getCalleeFunction because it is overly
    /// precise in finding the specific corresponding SILFunction. We only care
    /// about describing the referenced AST decl, since that's all programmers
    /// know.

    ValueDecl *callee = nullptr;

    auto inspect = [](ValueDecl *decl) -> ValueDecl* {
      // if this is an accessor, then return the storage instead.
      if (auto accessor = dyn_cast<AccessorDecl>(decl))
        return accessor->getStorage();

      return decl;
    };

    SILValue silCallee = apply.getCalleeOrigin();
    if (auto *methInst = dyn_cast<MethodInst>(silCallee))
      callee = inspect(methInst->getMember().getDecl());

    if (auto *funcInst = dyn_cast<FunctionRefBaseInst>(silCallee)) {
      auto *refFunc = funcInst->getInitiallyReferencedFunction();

      if (auto *declCxt = refFunc->getDeclContext()) {
        if (auto *absFn =
            dyn_cast_or_null<AbstractFunctionDecl>(declCxt->getAsDecl())) {
          callee = inspect(absFn);
        } else if (isa<AbstractClosureExpr>(declCxt)) {
          // TODO: determine if the closure captures self, or is applied to it,
          // so we can be more specific in this message.
          return std::make_tuple("this closure involving", "", ctx.Id_self);
        }
      }
    }

    // if we have no callee info, all we know is it's a call involving self.
    if (!callee)
      return std::make_tuple("a call involving", "", ctx.Id_self);

    // otherwise, form the tuple relative to the callee decl.
    return std::make_tuple(
      verbForInvoking(callee),
      callee->getDescriptiveKindName(callee->getDescriptiveKind()),
      callee->getName()
    );
  }

  // handle other non-call blames.
  switch (blame->getKind()) {
    case SILInstructionKind::CopyValueInst:
      return std::make_tuple("making a copy of", "", ctx.Id_self);
    default:
      return std::make_tuple("this use of", "", ctx.Id_self);
  }
}

/// Emits errors for all isolated uses of `self` in the given block.
/// \param blame the instruction to blame for introducing non-isolation.
/// If not provided, a suitable instruction will be automatically found using a
/// search.
/// \param info the AnalysisInfo corresponding to the function containing this
/// block.
void Info::diagnoseAll(AnalysisInfo &info, bool forDeinit,
                       SILInstruction* blame) {
  if (propertyUses.empty())
    return;

  auto *fn = info.getFunction();
  auto &ctx = fn->getASTContext();

  // Disable these diagnostics in deinitializers unless complete checking is
  // enabled.
  if (forDeinit && ctx.LangOpts.StrictConcurrencyLevel
        != StrictConcurrency::Complete)
    return;

  // Blame that is valid for the first property use is valid for all uses
  // in this block.
  if (!blame)
    blame = info.findNonisolatedBlame(firstPropertyUse());

  // if needed, find the blame inside of the defer callee.
  if (auto *callee = getCallee(blame)) {
    if (info.haveDeferInfo(callee)) {
      auto &defer = info.getOrCreateDeferInfo(callee);
      assert(defer.normalReturn && "noreturn defer should never be blamed!");

      auto *retBlk = defer.normalReturn->first;
      blame = defer.findNonisolatedBlame(retBlk->getTerminator());
    }
  }

  auto &diag = fn->getASTContext().Diags;

  SILLocation blameLoc = blame->getDebugLocation().getLocation();

  for (auto *use : propertyUses) {
    // If the illegal use is a call to a defer, then recursively diagnose
    // all of the defer's uses, if this is the first time encountering it.
    if (auto *callee = getCallee(use)) {
      assert(info.haveDeferInfo(callee) && "non-defer call as a property use?");
      auto &defer = info.getOrCreateDeferInfo(callee);
      if (defer.setNonisolatedStart()) {
        defer.diagnoseEntireFunction(blame);
      }
      continue;
    }

    assert(isa<RefElementAddrInst>(use) && "only expecting one kind of instr.");

    SILLocation illegalLoc = use->getDebugLocation().getLocation();
    VarDecl *var = cast<RefElementAddrInst>(use)->getField();

    diag.diagnose(illegalLoc.getSourceLoc(), diag::isolated_after_nonisolated,
                  forDeinit, var->getDescriptiveKind(), var->getName())
      .highlight(illegalLoc.getSourceRange())
      .warnUntilSwiftVersion(6);

    // after <verb><adjective> <subject>, ... can't use self anymore, etc ...
    //   example:
    // after calling function 'hello()', ...
    StringRef verb;
    StringRef adjective;
    DeclName subject;
    std::tie(verb, adjective, subject) = describe(blame);

    diag.diagnose(blameLoc.getSourceLoc(), diag::nonisolated_blame,
                  forDeinit, verb, adjective, subject)
      .highlight(blameLoc.getSourceRange());
  }
}

// MARK: analysis

/// \returns true iff the access is concurrency-safe in a nonisolated context
/// without an await.
static bool accessIsConcurrencySafe(ModuleDecl *module,
                                    RefElementAddrInst *inst) {
  VarDecl *var = inst->getField();

  // must be accessible from nonisolated and Sendable
  return isLetAccessibleAnywhere(module, var)
      && isSendableType(module, var->getTypeInContext());
}

/// \returns true iff the ref_element_addr instruction is only used
/// to deinitialize the referenced element.
static bool onlyDeinitAccess(RefElementAddrInst *inst) {
  if (auto operand = inst->getSingleUse()) {
    if (auto *access = dyn_cast<BeginAccessInst>(operand->getUser())) {
      return access->getAccessKind() == SILAccessKind::Deinit;
    }
  }
  return false;
}

/// Checks that the accessed element conforms to Sendable; emitting a
/// diagnostic if it is not Sendable. The diagnostic assumes that the access
/// is happening in a deinit that uses flow-isolation.
/// \returns true iff a diagnostic was emitted for this reference.
static bool diagnoseNonSendableFromDeinit(ModuleDecl *module,
                                          RefElementAddrInst *inst) {
  VarDecl *var = inst->getField();
  Type ty = var->getTypeInContext();
  DeclContext* dc = inst->getFunction()->getDeclContext();

// FIXME: we should emit diagnostics in other modes using:
//
//  if (!shouldDiagnoseExistingDataRaces(dc))
//    return false;
//
// but until we decide how we want to handle isolated state from deinits,
// we're going to limit the noise to complete mode for now.
  if (dc->getASTContext().LangOpts.StrictConcurrencyLevel
      != StrictConcurrency::Complete)
      return false;

  if (isSendableType(module, ty))
    return false;

  auto &diag = var->getASTContext().Diags;

  diag.diagnose(inst->getLoc().getSourceLoc(), diag::non_sendable_from_deinit,
                ty, var->getDescriptiveKind(), var->getName())
      .warnUntilSwiftVersion(6);

  return true;
}

/// Analyzes a function for uses of `self` and records the kinds of isolation
/// required.
/// \param selfParam the parameter of \c getFunction() that should be
/// treated as \c self
void AnalysisInfo::analyze(const SILArgument *selfParam) {
  assert(selfParam && "analyzing a function with no self?");

  ModuleDecl *module = getFunction()->getModule().getSwiftModule();

  // Use a worklist to track the uses left to be searched.
  SmallVector<Operand *, 32> worklist;

  // Seed with direct users of `self`
  worklist.append(selfParam->use_begin(), selfParam->use_end());

  while (!worklist.empty()) {
    Operand *operand = worklist.pop_back_val();
    SILInstruction *user = operand->getUser();

    // First, check if this is an apply that involves `self`
    if (auto apply = ApplySite::isa(user)) {

      // Check if the callee is a function representing a defer block.
      if (SILFunction *callee = apply.getCalleeFunction()) {
        if (auto *dc = callee->getDeclContext()) {
          if (auto *decl = dyn_cast_or_null<FuncDecl>(dc->getAsDecl())) {
            if (decl->isDeferBody()) {

              // If we need to analyze the defer first, do so.
              if (!haveDeferInfo(callee)) {
                // NOTE: the defer function is not like a method, because it
                // doesn't satisfy hasSelfParam().
                auto const* calleeSelfParam =
                  callee->getArgument(apply.getAppliedArgIndex(*operand));

                // Recursion depth is bounded by the lexical nesting of
                // defer blocks in the input program.
                auto &defer = getOrCreateDeferInfo(callee);
                defer.analyze(calleeSelfParam);
                defer.solve();
              }

              auto const& defer = getOrCreateDeferInfo(callee);

              // A defer effectively has one exit block, because it can't throw.
              // Otherwise, it may never return (e.g., fatalError).
              // So, we say that this instruction generates nonisolation only
              // if it can return normally, and if it does, it carries
              // nonisolation.
              if (defer.normalReturn) {
                if (defer.normalReturn->second == State::Nonisolated) {
                  markNonIsolated(user);
                }
              }

              // If the defer body has any stored property uses, we record that
              // in the parent by declaring this call-site being a property use.
              if (defer.hasPropertyUse())
                markPropertyUse(user);

              continue;
            }
          }
        }
      }

      // For all other call-sites, uses of `self` are nonisolated.
      markNonIsolated(user);
      continue;
    }

    // Handle non-ApplySite instructions.
    switch (user->getKind()) {
      // Look for a property access.
      // Sadly, formal accesses are not always emitted by SILGen, particularly,
      // within the initializers we care about. So we rely on ref_element_addr.
      case SILInstructionKind::RefElementAddrInst: {
        RefElementAddrInst *refInst = cast<RefElementAddrInst>(user);

        // skip auto-generated deinit accesses.
        if (onlyDeinitAccess(refInst))
          continue;

        // skip known-safe accesses.
        if (accessIsConcurrencySafe(module, refInst))
          continue;

        // emit a diagnostic and skip if it's non-sendable in a deinit
        if (forDeinit && diagnoseNonSendableFromDeinit(module, refInst))
          continue;

        markPropertyUse(user);
        break;
      }

      // Look through certian kinds of single-value instructions.
      case SILInstructionKind::CopyValueInst:
        // TODO: If we had some actual escape analysis information, we could
        // avoid marking a trivial copy as a nonisolated use, since it doesn't
        // actually escape the function. We have to be conservative here
        // and assume it might.
        markNonIsolated(user);
        break;

      case SILInstructionKind::BeginAccessInst:
      case SILInstructionKind::BeginBorrowInst: {
        auto *svi = cast<SingleValueInstruction>(user);
        worklist.append(svi->use_begin(), svi->use_end());
        break;
      }

      default:
        // don't follow this instruction.
        LLVM_DEBUG(llvm::dbgs() << DEBUG_TYPE << " def-use walk skipping: "
                       << *user);
        break;
    }
  }
}

/// Initialize and solve the dataflow problem, assuming the entry block starts
/// isolated.
void AnalysisInfo::solve() {
  SILFunction *fn = getFunction();
  SILBasicBlock *returnBlk = nullptr;

  // NOTE: if the starting isolation is nonisolated, the solution is trivial.
  // Since we don't expect calls to solve in that situation, I haven't
  // implemented that

  // Initialize the forward dataflow problem.
  for (auto pair : flow) {
    SILBasicBlock *blk = &(pair.block);
    auto &data = pair.data;

    // record the return block
    if (isa<ReturnInst>(blk->getTerminator())) {
      assert(returnBlk == nullptr); // should only be one!
      returnBlk = blk;
    }

    // Set everything to Bottom.
    data.entrySet.reset();
    data.genSet.reset();
    data.killSet.reset();
    data.exitSet.reset();

    if (blk == fn->getEntryBlock())
      data.entrySet.set(startingIsolation);

    // A nonisolated use "kills" isolation and generates nonisolation.
    if (this->operator[](blk).hasNonisolatedUse()) {
      data.killSet.set(State::Isolated);
      data.genSet.set(State::Nonisolated);
    }
  }

  // Solve using a union so that Top represents a conflict.
  flow.solveForwardWithUnion();

  // If this function can return normally, update the outgoing isolation
  // in that case. This is needed to implement `defer`.
  if (returnBlk) {
    auto &returnInfo = flow[returnBlk];
    if (returnInfo.exitSet[State::Nonisolated])
      normalReturn = std::make_pair(returnBlk, State::Nonisolated);
    else
      normalReturn = std::make_pair(returnBlk, State::Isolated);
  }
}

/// Enforces isolation rules, given the flow and block-local information.
void AnalysisInfo::verifyIsolation() {
  // go through all the blocks.
  for (auto entry : *this) {
    auto &block = entry.block;
    auto &data = entry.data;
    auto &flowInfo = flow[&block];

    // If the block has no isolated uses, then skip it.
    if (data.propertyUses.empty())
      continue;

    // If flow-analysis determined that we might be `nonisolated` coming
    // into this block, then all isolated uses in this block are invalid.
    if (flowInfo.entrySet[State::Nonisolated]) {
      data.diagnoseAll(*this, forDeinit);
      continue;
    }

    // Otherwise, we must be starting off isolated.
    assert(flowInfo.entrySet[State::Isolated]);

    // If this block doesn't introduce nonisolation, then we can skip it.
    if (data.nonisolatedUses.empty()) {
      // make sure flow analysis agrees.
      assert(flowInfo.exitSet[State::Nonisolated] == 0);
      continue;
    }

    // Finally, we must scan the block to determine which isolated uses
    // are illegal. If isolated uses appear after nonisolated ones, then
    // that is an error. So, our strategy is to remove the valid isolated
    // uses, until we find the first nonisolated use. Then, we can simply
    // diagnose the remaining uses.
    SILInstruction *nonisolatedUse = nullptr;
    auto current = block.begin();
    while (current != block.end()) {
      SILInstruction *inst = &*current;

      auto result = data.nonisolatedUses.find(inst);
      if (result != data.nonisolatedUses.end()) {
        nonisolatedUse = *result;
        break;
      }

      data.propertyUses.erase(inst);
      current++;
    }

    assert(nonisolatedUse && "should have found a use!");
    data.diagnoseAll(*this, forDeinit, nonisolatedUse);
  }

  // recursively verify isolation of defer functions.
  for (auto &entry : deferBlocks) {
    // skip those with nonisolated start, since we've already diagnosed those.
    if (entry.second->hasNonisolatedStart())
      continue;

    entry.second->verifyIsolation();
  }
}

// MARK: high-level setup

/// Performs flow-sensitive actor-isolation checking on the given SILFunction.
void checkFlowIsolation(SILFunction *fn) {
  assert(fn->hasSelfParam() && "cannot analyze without a self param!");

  // Step 1 -- Analyze uses of `self` within the function.
  AnalysisInfo info(fn);
  info.analyze(fn->getSelfArgument());

  // Step 2 -- Initialize and solve the dataflow problem.
  info.solve();

  LLVM_DEBUG(info.dump());

  // Step 3 -- With the information gathered, check for flow-isolation issues.
  info.verifyIsolation();
}

/// The FlowIsolation pass performs flow-sensitive actor-isolation checking in
/// the body of actor member functions that treat `self` as `nonisolated` after
/// the first `nonisolated` use. This pass uses a simple forward dataflow
/// analysis to track these changes and emits diagnostics if an isolated use of
/// `self` appears when `self` may be `nonisolated` at that point in the
/// function.
class FlowIsolation : public SILFunctionTransform {

  /// The entry point to the checker.
  void run() override {
    SILFunction *fn = getFunction();

    // Don't rerun diagnostics on deserialized functions.
    if (fn->wasDeserializedCanonical())
      return;

    // Look for functions that use flow-isolation.
    if (auto *dc = fn->getDeclContext())
      if (auto *afd = dyn_cast_or_null<AbstractFunctionDecl>(dc->getAsDecl()))
        if (usesFlowSensitiveIsolation(afd))
          checkFlowIsolation(fn);

    return;
  }

}; // class

} // anonymous namespace

/// This pass is known to depend on the following passes having run before it:
///   - NoReturnFolding
SILTransform *swift::createFlowIsolation() {
  return new FlowIsolation();
}
