//===--- SILProfiler.cpp - Instrumentation based profiling ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILProfiler.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/Parse/Lexer.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/SILModule.h"

#define DEBUG_TYPE "SILProfiler"

using namespace swift;

/// Unfortunately this is needed as ASTNode can't currently represent a
/// SourceFile.
class NodeToProfile final {
  /// For a direct ASTNode, this stores the node itself. For a main SourceFile,
  /// it stores the corresponding ModuleDecl.
  ASTNode Storage;

  explicit NodeToProfile(ASTNode Node) : Storage(Node) {}

public:
  static NodeToProfile node(ASTNode Node) {
    assert(!isa_and_nonnull<ModuleDecl>(Node.dyn_cast<Decl *>()));
    return NodeToProfile(Node);
  }
  static NodeToProfile mainSourceFile(SourceFile *SF) {
    assert(SF->isScriptMode());
    auto N = NodeToProfile(SF->getParentModule());
    assert(N.getAsSourceFile() == SF);
    return N;
  }

  /// If an ASTNode is being stored, returns it, otherwise \c nullptr.
  ASTNode getAsNode() const {
    return isSourceFile() ? nullptr : Storage;
  }

  /// Whether this is storing a main SourceFile.
  bool isSourceFile() const {
    return getAsSourceFile();
  }

  /// If a main SourceFile is being stored, returns it, otherwise \c nullptr.
  SourceFile *getAsSourceFile() const {
    auto *M = dyn_cast_or_null<ModuleDecl>(Storage.dyn_cast<Decl *>());
    return M ? &M->getMainSourceFile() : nullptr;
  }
};

static NodeToProfile getNodeToProfile(SILDeclRef Constant) {
  // If we have an initialization expression, walk that instead of the variable.
  if (auto *E = Constant.getInitializationExpr())
    return NodeToProfile::node(E);

  // Otherwise, we walk the SILDeclRef's node directly.
  using LocKind = SILDeclRef::LocKind;
  switch (Constant.getLocKind()) {
  case LocKind::Decl:
    return NodeToProfile::node(Constant.getDecl());
  case LocKind::Closure:
    return NodeToProfile::node(Constant.getAbstractClosureExpr());
  case LocKind::File: {
    auto *SF = cast<SourceFile>(Constant.getFileUnit());
    return NodeToProfile::mainSourceFile(SF);
  }
  }
  llvm_unreachable("Unhandled case in switch!");
}

/// Check whether we should profile a given SILDeclRef.
static bool shouldProfile(SILDeclRef Constant) {
  auto Root = getNodeToProfile(Constant);
  auto *DC = Constant.getInnermostDeclContext();

  if (auto N = Root.getAsNode()) {
    // Do not profile AST nodes with invalid source locations.
    if (N.getStartLoc().isInvalid() || N.getEndLoc().isInvalid()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Skipping ASTNode: invalid start/end locations\n");
      return false;
    }

    // Do not profile generated code. This includes macro expansions, which we
    // otherwise consider to be "written by the user", because they wrote the
    // macro attribute or expr. We may want to revist this in the future. We'll
    // need to figure out how we'll be writing out the macro expansions though,
    // such that they can be referenced by llvm-cov.
    // Note we check `getSourceFileContainingLocation` instead of
    // `getParentSourceFile` to make sure initializer exprs are correctly
    // handled.
    auto *M = DC->getParentModule();
    if (auto *SF = M->getSourceFileContainingLocation(N.getStartLoc())) {
      auto &SM = M->getASTContext().SourceMgr;
      if (SM.hasGeneratedSourceInfo(*SF->getBufferID())) {
        LLVM_DEBUG(llvm::dbgs() << "Skipping ASTNode: generated code\n");
        return false;
      }
    }
  }

  // Do not profile AST nodes in unavailable contexts.
  if (auto *D = DC->getInnermostDeclarationDeclContext()) {
    if (D->getSemanticUnavailableAttr()) {
      LLVM_DEBUG(llvm::dbgs() << "Skipping ASTNode: unavailable context\n");
      return false;
    }
  }

  // Do not profile code that hasn't been written by the user.
  if (!Constant.hasUserWrittenCode()) {
    LLVM_DEBUG(llvm::dbgs() << "Skipping ASTNode: no user-written code\n");
    return false;
  }

  return true;
}

static Stmt *getProfilerStmtForCase(CaseStmt *caseStmt) {
  switch (caseStmt->getParentKind()) {
  case CaseParentKind::Switch:
    return caseStmt;
  case CaseParentKind::DoCatch:
    return caseStmt->getBody();
  }
  llvm_unreachable("invalid parent kind");
}

SILProfiler *SILProfiler::create(SILModule &M, SILDeclRef Ref) {
  // If profiling isn't enabled, don't profile anything.
  const auto &Opts = M.getOptions();
  if (!Opts.GenerateProfile && Opts.UseProfile.empty())
    return nullptr;

  if (!shouldProfile(Ref))
    return nullptr;

  auto *Buf = M.allocate<SILProfiler>(1);
  auto *SP = ::new (Buf) SILProfiler(M, Ref, Opts.EmitProfileCoverageMapping);
  SP->assignRegionCounters();
  return SP;
}

static SILLocation getLocation(ASTNode Node) {
  if (auto *E = Node.dyn_cast<Expr *>()) {
    return E;
  } else if (auto *S = Node.dyn_cast<Stmt *>()) {
    return S;
  } else if (auto *D = Node.dyn_cast<Decl *>()) {
    return D;
  }
  llvm_unreachable("unsupported ASTNode");
}

SILLocation ProfileCounterRef::getLocation() const {
  return ::getLocation(Node);
}

void ProfileCounterRef::dumpSimple(raw_ostream &OS) const {
  switch (RefKind) {
  case Kind::Node: {
    OS << Node.getOpaqueValue() << " ";
    if (auto *D = Node.dyn_cast<Decl *>()) {
      OS << Decl::getKindName(D->getKind());
    } else if (auto *E = Node.dyn_cast<Expr *>()) {
      OS << Decl::getKindName(D->getKind());
    } else if (auto *S = Node.dyn_cast<Stmt *>()) {
      OS << Stmt::getKindName(S->getKind());
    }
  }
  }
}

void ProfileCounterRef::dump(raw_ostream &OS) const {
  switch (RefKind) {
  case Kind::Node:
    Node.dump(OS);
  }
}

void ProfileCounterRef::dump() const {
  dump(llvm::errs());
}

namespace {

/// Special logic for handling function visitation.
///
/// To avoid creating duplicate mappings, a function decl is only profiled if
/// it hasn't been reached via recursive walk.
///
/// Apply \p Func if the function can be visited.
template <typename F>
ASTWalker::PreWalkAction
visitFunctionDecl(ASTWalker &Walker, AbstractFunctionDecl *AFD, F Func) {
  if (Walker.Parent.isNull()) {
    assert(AFD->hasBody());
    Func();
    return ASTWalker::Action::Continue();
  }
  return ASTWalker::Action::SkipChildren();
}

/// Whether to walk the children of a given expression.
ASTWalker::PreWalkResult<Expr *>
shouldWalkIntoExpr(Expr *E, ASTWalker::ParentTy Parent, SILDeclRef Constant) {
  using Action = ASTWalker::Action;

  // Profiling for closures should be handled separately. Do not visit
  // closure expressions twice.
  if (auto *CE = dyn_cast<AbstractClosureExpr>(E)) {
    // A non-null parent means we have a closure child, which we will visit
    // separately. Even if the parent is null, don't walk into a closure if the
    // SILDeclRef is not for a closure, as it could be for a property
    // initializer instead.
    if (!Parent.isNull() || !Constant || !Constant.getAbstractClosureExpr())
      return Action::SkipChildren(E);
  }
  return Action::Continue(E);
}

/// Whether to skip visitation of an expression. The children may however still
/// be visited
bool shouldSkipExpr(Expr *E) {
  // Expressions with no location should be skipped.
  return E->getStartLoc().isInvalid() || E->getEndLoc().isInvalid();
}

/// Whether the children of a decl that isn't explicitly handled should be
/// walked.
static bool shouldWalkIntoUnhandledDecl(const Decl *D) {
  // We want to walk into the initializer for a pattern binding decl. This
  // allows us to map LazyInitializerExprs.
  return isa<PatternBindingDecl>(D);
}

/// An ASTWalker that maps ASTNodes to profiling counters.
struct MapRegionCounters : public ASTWalker {
  /// The SIL function being profiled.
  SILDeclRef Constant;

  /// The next counter value to assign.
  unsigned NextCounter = 0;

  /// The map of statements to counters.
  llvm::DenseMap<ProfileCounterRef, unsigned> &CounterMap;

  MapRegionCounters(SILDeclRef Constant,
                    llvm::DenseMap<ProfileCounterRef, unsigned> &CounterMap)
      : Constant(Constant), CounterMap(CounterMap) {}

  LazyInitializerWalking getLazyInitializerWalkingBehavior() override {
    // We want to walk lazy initializers present in the synthesized getter for
    // a lazy variable.
    return LazyInitializerWalking::InAccessor;
  }

  void mapRegion(ASTNode N) {
    mapRegion(ProfileCounterRef::node(N));
  }

  void mapRegion(ProfileCounterRef Ref) {
    CounterMap[Ref] = NextCounter;

    LLVM_DEBUG({
      llvm::dbgs() << "Assigned counter #" << NextCounter << " to: ";
      Ref.dumpSimple(llvm::dbgs());
      llvm::dbgs() << "\n";
    });

    ++NextCounter;
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(D)) {
      return visitFunctionDecl(*this, AFD, [&] { mapRegion(AFD->getBody()); });
    } else if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
      mapRegion(TLCD->getBody());
      return Action::Continue();
    }
    return Action::VisitChildrenIf(shouldWalkIntoUnhandledDecl(D));
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (auto *IS = dyn_cast<IfStmt>(S)) {
      mapRegion(IS->getThenStmt());
    } else if (auto *US = dyn_cast<GuardStmt>(S)) {
      mapRegion(US->getBody());
    } else if (auto *WS = dyn_cast<WhileStmt>(S)) {
      mapRegion(WS->getBody());
    } else if (auto *RWS = dyn_cast<RepeatWhileStmt>(S)) {
      mapRegion(RWS->getBody());
    } else if (auto *FES = dyn_cast<ForEachStmt>(S)) {
      mapRegion(FES->getBody());
    } else if (auto *CS = dyn_cast<CaseStmt>(S)) {
      mapRegion(getProfilerStmtForCase(CS));
    }
    return Action::Continue(S);
  }

  PreWalkAction walkToParameterListPre(ParameterList *PL) override {
    // We don't walk into parameter lists. Default arguments should be visited
    // directly.
    // FIXME: We don't yet profile default argument generators at all.
    return Action::SkipChildren();
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (shouldSkipExpr(E))
      return shouldWalkIntoExpr(E, Parent, Constant);

    // If AST visitation begins with an expression, the counter map must be
    // empty. Set up a counter for the root.
    if (Parent.isNull()) {
      assert(CounterMap.empty() && "Mapped a region before visiting the root?");
      mapRegion(E);
    }

    if (auto *IE = dyn_cast<TernaryExpr>(E)) {
      mapRegion(IE->getThenExpr());
    }

    if (isa<LazyInitializerExpr>(E))
      mapRegion(E);

    return shouldWalkIntoExpr(E, Parent, Constant);
  }
};

struct CounterExprStorage;
using CounterAllocator = llvm::SpecificBumpPtrAllocator<CounterExprStorage>;

/// A node in an expression tree of counters.
class CounterExpr {
  enum class Kind { Leaf, Add, Sub, Zero };
  Kind K;
  Optional<ProfileCounterRef> Counter;
  const CounterExprStorage *Storage = nullptr;

  CounterExpr(Kind K) : K(K) {
    assert((K == Kind::Zero) && "only valid for Zero");
  }

  CounterExpr(Kind K, ProfileCounterRef Counter) : K(K), Counter(Counter) {
    assert(K == Kind::Leaf && "only valid for Node");
  }

  CounterExpr(Kind K, const CounterExprStorage *Storage)
      : K(K), Storage(Storage) {
    assert((K == Kind::Add || K == Kind::Sub) && "only valid for operators");
  }

public:
  static CounterExpr Leaf(ProfileCounterRef Counter) {
    return CounterExpr(Kind::Leaf, Counter);
  }
  static CounterExpr Zero() {
    return CounterExpr(Kind::Zero);
  }

  static CounterExpr Add(CounterExpr LHS, CounterExpr RHS,
                         CounterAllocator &Alloc);
  static CounterExpr Sub(CounterExpr LHS, CounterExpr RHS,
                         CounterAllocator &Alloc);

  /// Returns true if this is a Zero node.
  bool isZero() const { return K == Kind::Zero; }

  /// For an addition or subtraction counter, retrieves the LHS counter.
  const CounterExpr &getLHS() const;

  /// For an addition or subtraction counter, retrieves the RHS counter.
  const CounterExpr &getRHS() const;

  /// Returns true if the counter is semantically a Zero node. This considers
  /// the simplified version of the counter that has eliminated redundant
  /// operations.
  bool isSemanticallyZero() const {
    // Run the counter through the counter builder to simplify it, using a dummy
    // mapping of unique counter indices for each node reference. The value of
    // the indices doesn't matter, but we need to ensure that e.g subtraction
    // of a node from itself cancels out.
    llvm::coverage::CounterExpressionBuilder Builder;
    llvm::DenseMap<ProfileCounterRef, unsigned> DummyIndices;
    unsigned LastIdx = 0;
    auto Counter = expand(Builder, [&](auto Ref) {
      if (!DummyIndices.count(Ref)) {
        DummyIndices[Ref] = LastIdx;
        LastIdx += 1;
      }
      return DummyIndices[Ref];
    });
    return Counter.isZero();
  }

  /// Expand this node into an llvm::coverage::Counter.
  ///
  /// Updates \c Builder with any expressions that are needed to represent this
  /// counter.
  llvm::coverage::Counter
  expand(llvm::coverage::CounterExpressionBuilder &Builder,
         llvm::function_ref<unsigned(ProfileCounterRef)> GetCounterIdx) const {
    switch (K) {
    case Kind::Zero:
      return llvm::coverage::Counter::getZero();
    case Kind::Leaf:
      return llvm::coverage::Counter::getCounter(GetCounterIdx(*Counter));
    case Kind::Add:
      return Builder.add(getLHS().expand(Builder, GetCounterIdx),
                         getRHS().expand(Builder, GetCounterIdx));
    case Kind::Sub:
      return Builder.subtract(getLHS().expand(Builder, GetCounterIdx),
                              getRHS().expand(Builder, GetCounterIdx));
    }

    llvm_unreachable("Unhandled Kind in switch.");
  }

  /// Expand this node into an llvm::coverage::Counter.
  ///
  /// Updates \c Builder with any expressions that are needed to represent this
  /// counter.
  llvm::coverage::Counter
  expand(llvm::coverage::CounterExpressionBuilder &Builder,
         const llvm::DenseMap<ProfileCounterRef, unsigned> &Counters) const {
    return expand(Builder, [&](auto Ref) {
      auto Result = Counters.find(Ref);
      assert(Result != Counters.end() && "Counter not found");
      return Result->second;
    });
  }

  void print(raw_ostream &OS) const {
    switch (K) {
    case Kind::Zero:
      OS << "zero";
      return;
    case Kind::Leaf:
      OS << "leaf(";
      Counter->dumpSimple(OS);
      OS << ")";
      return;
    case Kind::Add:
    case Kind::Sub:
      getLHS().print(OS);
      OS << ' ' << ((K == Kind::Add) ? '+' : '-') << ' ';
      getRHS().print(OS);
      return;
    }
    llvm_unreachable("Unhandled Kind in switch.");
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const { print(llvm::errs()); }
#endif
};

struct CounterExprStorage {
  CounterExpr LHS;
  CounterExpr RHS;
};

inline CounterExpr CounterExpr::Add(CounterExpr LHS, CounterExpr RHS,
                                    CounterAllocator &Alloc) {
  auto *Storage = Alloc.Allocate();
  Storage->LHS = LHS;
  Storage->RHS = RHS;
  return CounterExpr(Kind::Add, Storage);
}
inline CounterExpr CounterExpr::Sub(CounterExpr LHS, CounterExpr RHS,
                                    CounterAllocator &Alloc) {
  auto *Storage = Alloc.Allocate();
  Storage->LHS = LHS;
  Storage->RHS = RHS;
  return CounterExpr(Kind::Sub, Storage);
}

inline const CounterExpr &CounterExpr::getLHS() const {
  assert(Storage && "Counter does not have an LHS");
  return Storage->LHS;
}

inline const CounterExpr &CounterExpr::getRHS() const {
  assert(Storage && "Counter does not have an RHS");
  return Storage->RHS;
}

/// A region of source code that can be mapped to a counter.
class SourceMappingRegion {
  ASTNode Node;

  /// The counter for an incomplete region. Note we do not store counters
  /// for nodes, as we need to be able to fix them up after popping the regions.
  Optional<CounterExpr> Counter;

  /// The region's starting location.
  Optional<SourceLoc> StartLoc;

  /// The region's ending location.
  Optional<SourceLoc> EndLoc;

public:
  SourceMappingRegion(ASTNode Node, Optional<CounterExpr> Counter,
                      Optional<SourceLoc> StartLoc, Optional<SourceLoc> EndLoc)
      : Node(Node), Counter(std::move(Counter)), StartLoc(StartLoc),
        EndLoc(EndLoc) {
    assert((!StartLoc || StartLoc->isValid()) &&
           "Expected start location to be valid");
    assert((!EndLoc || EndLoc->isValid()) &&
           "Expected end location to be valid");
  }

  SourceMappingRegion(SourceMappingRegion &&Region) = default;
  SourceMappingRegion &operator=(SourceMappingRegion &&RHS) = default;

  ASTNode getNode() const { return Node; }

  CounterExpr getCounter(const llvm::DenseMap<ProfileCounterRef, CounterExpr>
                             &NodeCounters) const {
    if (Counter)
      return *Counter;

    auto Iter = NodeCounters.find(ProfileCounterRef::node(Node));
    assert(Iter != NodeCounters.end() && "Must have counter for node");
    return Iter->second;
  }

  bool hasStartLoc() const { return StartLoc.has_value(); }

  void setStartLoc(SourceLoc Loc) {
    assert(Loc.isValid());
    StartLoc = Loc;
  }

  const SourceLoc &getStartLoc() const {
    assert(StartLoc && "Region has no start location");
    return *StartLoc;
  }

  bool hasEndLoc() const { return EndLoc.has_value(); }

  void setEndLoc(SourceLoc Loc) {
    assert(Loc.isValid());
    EndLoc = Loc;
  }

  const SourceLoc &getEndLoc() const {
    assert(EndLoc && "Region has no end location");
    return *EndLoc;
  }

  void print(llvm::raw_ostream &OS, const SourceManager &SM) const {
    OS << "[";
    if (hasStartLoc())
      getStartLoc().print(OS, SM);
    else
      OS << "?";
    OS << ", ";
    if (hasEndLoc())
      getEndLoc().print(OS, SM);
    else
      OS << "?";
    OS << "]";
  }
};

/// An ASTWalker that maps ASTNodes to profiling counters.
///
/// TODO: We ought to be able to leverage the CounterExprs from the
/// CoverageMapping walker to recompute the correct counter information
/// for this walker.
struct PGOMapping : public ASTWalker {
  /// The SIL function being profiled.
  SILDeclRef Constant;

  /// The counter indices for AST nodes.
  const llvm::DenseMap<ProfileCounterRef, unsigned> &CounterMap;

  /// The loaded counter data.
  const llvm::InstrProfRecord &LoadedCounts;

  /// The output map of statements to counters.
  llvm::DenseMap<ProfileCounterRef, ProfileCounter> &LoadedCounterMap;
  llvm::DenseMap<ASTNode, ASTNode> &CondToParentMap;

  PGOMapping(SILDeclRef Constant,
             const llvm::DenseMap<ProfileCounterRef, unsigned> &CounterMap,
             const llvm::InstrProfRecord &LoadedCounts,
             llvm::DenseMap<ProfileCounterRef, ProfileCounter> &LoadedCounterMap,
             llvm::DenseMap<ASTNode, ASTNode> &RegionCondToParentMap)
      : Constant(Constant), CounterMap(CounterMap), LoadedCounts(LoadedCounts),
        LoadedCounterMap(LoadedCounterMap),
        CondToParentMap(RegionCondToParentMap) {}

  /// Retrieve the counter index for a leaf counter.
  unsigned getCounterIndex(ProfileCounterRef Ref) const {
    auto result = CounterMap.find(Ref);
    assert(result != CounterMap.end() && "Unmapped node?");
    return result->second;
  }

  /// Retrieve the counter index for a leaf node.
  unsigned getCounterIndex(ASTNode Node) const {
    return getCounterIndex(ProfileCounterRef::node(Node));
  }

  unsigned getParentCounter() const {
    if (Parent.isNull())
      return 0;
    else if (Parent.getKind() == ASTWalker::ParentKind::Decl) {
      auto it = CounterMap.find(ProfileCounterRef::node(Parent.getAsDecl()));
      return (it != CounterMap.end()) ? it->getSecond() : 0;
    } else if (Parent.getKind() == ASTWalker::ParentKind::Stmt) {
      auto it = CounterMap.find(ProfileCounterRef::node(Parent.getAsStmt()));
      return (it != CounterMap.end()) ? it->getSecond() : 0;
    } else if (Parent.getKind() == ASTWalker::ParentKind::Expr) {
      auto it = CounterMap.find(ProfileCounterRef::node(Parent.getAsExpr()));
      return (it != CounterMap.end()) ? it->getSecond() : 0;
    }
    return 0;
  }

  ProfileCounter subtract(ProfileCounter L, ProfileCounter R) {
    if (!L.hasValue() || !R.hasValue()) {
      return L;
    }
    uint64_t LV = L.getValue();
    uint64_t RV = R.getValue();
    assert(LV >= RV && "Invalid counter subtraction");
    return LV - RV;
  }

  /// Load the execution count corresponding to \p Ref from a profile, if one
  /// is available.
  ProfileCounter loadExecutionCount(ProfileCounterRef Ref) {
    auto CounterIt = CounterMap.find(Ref);
    assert(CounterIt != CounterMap.end() &&
           "region does not have an associated counter");

    unsigned CounterIndexForFunc = CounterIt->second;
    return LoadedCounts.Counts[CounterIndexForFunc];
  }

  /// Load the execution count corresponding to \p Node from a profile, if one
  /// is available.
  ProfileCounter loadExecutionCount(ASTNode Node) {
    return loadExecutionCount(ProfileCounterRef::node(Node));
  }

  /// Record the execution count for a leaf ref.
  void setKnownExecutionCount(ProfileCounterRef Ref) {
    LoadedCounterMap[Ref] = loadExecutionCount(Ref);
  }

  /// Record the execution count for a leaf node.
  void setKnownExecutionCount(ASTNode Node) {
    setKnownExecutionCount(ProfileCounterRef::node(Node));
  }

  /// Record a computed execution count for a node.
  void setExecutionCount(ASTNode Node, ProfileCounter count) {
    LoadedCounterMap[ProfileCounterRef::node(Node)] = count;
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(D)) {
      return visitFunctionDecl(*this, AFD, [&] {
        setKnownExecutionCount(AFD->getBody());
      });
    }
    if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
      setKnownExecutionCount(TLCD->getBody());
      return Action::Continue();
    }
    return Action::VisitChildrenIf(shouldWalkIntoUnhandledDecl(D));
  }

  LazyInitializerWalking getLazyInitializerWalkingBehavior() override {
    // We want to walk lazy initializers present in the synthesized getter for
    // a lazy variable.
    return LazyInitializerWalking::InAccessor;
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    unsigned parent = getParentCounter();
    auto parentCount = LoadedCounts.Counts[parent];
    if (auto *IS = dyn_cast<IfStmt>(S)) {
      auto thenStmt = IS->getThenStmt();
      auto thenCount = loadExecutionCount(thenStmt);
      setExecutionCount(thenStmt, thenCount);
      if (auto elseStmt = IS->getElseStmt()) {
        auto count = parentCount;
        if (!parent) {
          auto thenVal = thenCount.getValue();
          for (auto pCount = getCounterIndex(thenStmt); pCount > 0; --pCount) {
            auto cCount = LoadedCounts.Counts[pCount];
            if (cCount > thenVal) {
              count = cCount;
              break;
            }
          }
        }
        setExecutionCount(elseStmt, subtract(count, thenCount));
        auto Cond = IS->getCond();
        for (const auto &elt : Cond) {
          if (elt.getKind() ==
              StmtConditionElement::ConditionKind::CK_PatternBinding) {
            CondToParentMap[elt.getInitializer()] = IS;
          }
        }
      }
    } else if (auto *GS = dyn_cast<GuardStmt>(S)) {
      auto guardBody = GS->getBody();
      auto guardCount = loadExecutionCount(guardBody);
      setExecutionCount(guardBody, guardCount);
      setExecutionCount(GS, subtract(parentCount, guardCount));
    } else if (auto *WS = dyn_cast<WhileStmt>(S)) {
      setKnownExecutionCount(WS->getBody());
      setExecutionCount(WS, parentCount);
    } else if (auto *RWS = dyn_cast<RepeatWhileStmt>(S)) {
      setKnownExecutionCount(RWS->getBody());
      setExecutionCount(RWS, parentCount);
    } else if (auto *FES = dyn_cast<ForEachStmt>(S)) {
      setKnownExecutionCount(FES->getBody());
      setExecutionCount(FES, parentCount);
    } else if (auto *CS = dyn_cast<CaseStmt>(S)) {
      setKnownExecutionCount(getProfilerStmtForCase(CS));
    }
    return Action::Continue(S);
  }

  PreWalkAction walkToParameterListPre(ParameterList *PL) override {
    // We don't walk into parameter lists. Default arguments should be visited
    // directly.
    // FIXME: We don't yet profile default argument generators at all.
    return Action::SkipChildren();
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (shouldSkipExpr(E))
      return shouldWalkIntoExpr(E, Parent, Constant);

    unsigned parent = getParentCounter();

    if (Parent.isNull())
      setKnownExecutionCount(E);

    if (auto *IE = dyn_cast<TernaryExpr>(E)) {
      auto thenExpr = IE->getThenExpr();
      auto thenCount = loadExecutionCount(thenExpr);
      setExecutionCount(thenExpr, thenCount);
      auto elseExpr = IE->getElseExpr();
      assert(elseExpr && "An if-expr must have an else subexpression");
      auto count = LoadedCounts.Counts[parent];
      if (!parent) {
        auto thenVal = thenCount.getValue();
        for (auto pCount = getCounterIndex(thenExpr); pCount > 0; --pCount) {
          auto cCount = LoadedCounts.Counts[pCount];
          if (cCount > thenVal) {
            count = cCount;
            break;
          }
        }
      }
      setExecutionCount(elseExpr, subtract(count, thenCount));
    }
    if (isa<LazyInitializerExpr>(E))
      setKnownExecutionCount(E);

    return shouldWalkIntoExpr(E, Parent, Constant);
  }
};

/// Produce coverage mapping information for a function. This involves taking
/// the counters computed by MapRegionCounters, and annotating the source with
/// regions that are defined in terms of those counters.
struct CoverageMapping : public ASTWalker {
private:
  const SourceManager &SM;

  /// The SIL function being profiled.
  SILDeclRef Constant;

  /// Allocator for counter expressions.
  CounterAllocator CounterAlloc;

  /// The map of statements to counter expressions.
  llvm::DenseMap<ProfileCounterRef, CounterExpr> CounterMap;

  /// The source mapping regions for this function.
  std::vector<SourceMappingRegion> SourceRegions;

  /// A stack of currently live regions.
  std::vector<SourceMappingRegion> RegionStack;

  /// A stack of active repeat-while loops.
  std::vector<RepeatWhileStmt *> RepeatWhileStack;

  /// A stack of active do-catch statements.
  std::vector<DoCatchStmt *> DoCatchStack;

  Optional<CounterExpr> ExitCounter;

  Stmt *ImplicitTopLevelBody = nullptr;

  /// Return true if \c Ref has an associated counter.
  bool hasCounter(ProfileCounterRef Ref) { return CounterMap.count(Ref); }

  /// Return true if \c Node has an associated counter.
  bool hasCounter(ASTNode Node) {
    return hasCounter(ProfileCounterRef::node(Node));
  }

  /// Return the region counter for \c Ref.
  ///
  /// This should only be called on references that have a dedicated counter.
  CounterExpr getCounter(ProfileCounterRef Ref) {
    auto Iter = CounterMap.find(Ref);
    assert(Iter != CounterMap.end() && "No counter found");
    return Iter->second;
  }

  /// Return the region counter for \c Node.
  ///
  /// This should only be called on statements that have a dedicated counter.
  CounterExpr getCounter(ASTNode Node) {
    return getCounter(ProfileCounterRef::node(Node));
  }

  /// Create a counter expression for \c Ref and add it to the map.
  void assignCounter(ProfileCounterRef Ref, CounterExpr Expr) {
    auto Res = CounterMap.insert({Ref, Expr});

    // Overwrite an existing assignment.
    if (!Res.second)
      Res.first->second = std::move(Expr);
  }

  /// Create a counter expression for \c Node and add it to the map.
  void assignCounter(ASTNode Node, CounterExpr Expr) {
    assignCounter(ProfileCounterRef::node(Node), std::move(Expr));
  }

  /// Create a counter expression referencing \c Ref's own counter. This must
  /// have been previously mapped by MapRegionCounters.
  CounterExpr assignKnownCounter(ProfileCounterRef Ref) {
    auto Counter = CounterExpr::Leaf(Ref);
    assignCounter(Ref, Counter);
    return Counter;
  }

  /// Create a counter expression referencing \c Node's own counter. This must
  /// have been previously mapped by MapRegionCounters.
  CounterExpr assignKnownCounter(ASTNode Node) {
    return assignKnownCounter(ProfileCounterRef::node(Node));
  }

  /// Add \c Expr to \c Node's counter.
  void addToCounter(ASTNode Node, CounterExpr Expr) {
    auto Counter = getCounter(Node);
    if (Counter.isZero()) {
      Counter = std::move(Expr);
    } else {
      Counter = CounterExpr::Add(Counter, std::move(Expr), CounterAlloc);
    }
    assignCounter(Node, Counter);
  }

  /// Subtract \c Expr from \c Node's counter.
  void subtractFromCounter(ASTNode Node, CounterExpr Expr) {
    auto Counter = getCounter(Node);
    assert(!Counter.isZero() && "Cannot create a negative counter");
    assignCounter(Node,
                  CounterExpr::Sub(Counter, std::move(Expr), CounterAlloc));
  }

  /// Return the current region's counter.
  CounterExpr getCurrentCounter() { return getRegion().getCounter(CounterMap); }

  /// Get the counter from the end of the most recent scope.
  CounterExpr getExitCounter() {
    assert(ExitCounter && "no exit counter available");
    return *ExitCounter;
  }

  /// Set the exit count so we can leave the scope related to \c Node
  ///
  /// Returns the delta of the count on entering \c Node and exiting, or null if
  /// there was no change.
  Optional<CounterExpr> setExitCount(ASTNode Node) {
    ExitCounter = getCurrentCounter();
    if (hasCounter(Node) && getRegion().getNode() != Node)
      return CounterExpr::Sub(getCounter(Node), *ExitCounter, CounterAlloc);
    return None;
  }

  /// Adjust the count for control flow when exiting a scope.
  void adjustForNonLocalExits(ASTNode Scope, Optional<CounterExpr> ControlFlowAdjust) {
    if (Parent.getAsDecl())
      return;

    Optional<CounterExpr> JumpsToLabel;
    Stmt *ParentStmt = Parent.getAsStmt();
    if (ParentStmt) {
      if (isa<DoCatchStmt>(ParentStmt))
        return;
      auto caseStmt = dyn_cast_or_null<CaseStmt>(ParentStmt);
      if (caseStmt && caseStmt->getParentKind() == CaseParentKind::DoCatch)
        return;
      if (auto *LS = dyn_cast<LabeledStmt>(ParentStmt))
        JumpsToLabel = getCounter(LS);
    }

    if (!ControlFlowAdjust && !JumpsToLabel)
      return;

    auto Count = getCurrentCounter();
    // Add the counts from jumps directly to the label (such as breaks)
    if (JumpsToLabel)
      Count = CounterExpr::Add(Count, *JumpsToLabel, CounterAlloc);
    // Now apply any adjustments for control flow.
    if (ControlFlowAdjust)
      Count = CounterExpr::Sub(Count, *ControlFlowAdjust, CounterAlloc);

    replaceCount(Count, getEndLoc(Scope));
  }

  /// Push a region covering \c Node onto the stack.
  void pushRegion(ASTNode Node) {
    // Note we don't store counters for nodes, as we need to be able to fix
    // them up later.
    RegionStack.emplace_back(Node, /*Counter*/ None, Node.getStartLoc(),
                             getEndLoc(Node));
    LLVM_DEBUG({
      llvm::dbgs() << "Pushed region: ";
      RegionStack.back().print(llvm::dbgs(), SM);
      llvm::dbgs() << "\n";
    });
  }

  /// Replace the current region at \p Start with a new counter. If \p Start is
  /// \c None, or the counter is semantically zero, an 'incomplete' region is
  /// formed, which is not recorded unless followed by additional AST nodes.
  void replaceCount(CounterExpr Counter, Optional<SourceLoc> Start) {
    // If the counter is semantically zero, form an 'incomplete' region with
    // no starting location. This prevents forming unreachable regions unless
    // there is a following statement or expression to extend the region.
    if (Start && Counter.isSemanticallyZero())
      Start = None;

    RegionStack.emplace_back(ASTNode(), Counter, Start, None);
  }

  /// Get the location for the end of the last token in \c Node.
  SourceLoc getEndLoc(ASTNode Node) {
    return Lexer::getLocForEndOfToken(SM, Node.getEndLoc());
  }

  /// Pop regions from the stack into the function's list of regions.
  ///
  /// Adds all regions from \c ParentNode to the top of the stack to the
  /// function's \c SourceRegions.
  void popRegions(ASTNode ParentNode) {
    auto I = RegionStack.begin(), E = RegionStack.end();
    while (I != E &&
           I->getNode().getOpaqueValue() != ParentNode.getOpaqueValue())
      ++I;
    assert(I != E && "parent not in stack");
    auto ParentIt = I;
    SourceLoc EndLoc = ParentIt->getEndLoc();

    unsigned FirstPoppedIndex = SourceRegions.size();
    (void)FirstPoppedIndex;
    SourceRegions.push_back(std::move(*I++));
    for (; I != E; ++I) {
      if (!I->hasStartLoc())
        continue;
      if (!I->hasEndLoc())
        I->setEndLoc(EndLoc);
      SourceRegions.push_back(std::move(*I));
    }

    LLVM_DEBUG({
      for (unsigned Idx = FirstPoppedIndex; Idx < SourceRegions.size(); ++Idx) {
        llvm::dbgs() << "Popped region: ";
        SourceRegions[Idx].print(llvm::dbgs(), SM);
        llvm::dbgs() << "\n";
      }
    });

    RegionStack.erase(ParentIt, E);
  }

  /// Return the currently active region.
  SourceMappingRegion &getRegion() {
    assert(!RegionStack.empty() && "statement has no region");
    return RegionStack.back();
  }

  /// Ensure that \c S is included in the current region.
  void extendRegion(ASTNode S) {
    SourceMappingRegion &Region = getRegion();
    SourceLoc StartLoc = S.getStartLoc();
    if (!Region.hasStartLoc())
      Region.setStartLoc(StartLoc);
  }

  /// Mark \c S as a terminator, starting a zero region.
  void terminateRegion(ASTNode S) {
    SourceMappingRegion &Region = getRegion();
    if (!Region.hasEndLoc())
      Region.setEndLoc(getEndLoc(S));
    replaceCount(CounterExpr::Zero(), /*Start*/ None);
  }

  Expr *getConditionNode(StmtCondition SC) {
    assert(!SC.empty() && "Empty condition");
    return SC.front().getBooleanOrNull();
  }

public:
  CoverageMapping(const SourceManager &SM, SILDeclRef Constant)
      : SM(SM), Constant(Constant) {}

  LazyInitializerWalking getLazyInitializerWalkingBehavior() override {
    // We want to walk lazy initializers present in the synthesized getter for
    // a lazy variable.
    return LazyInitializerWalking::InAccessor;
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  /// Generate the coverage counter mapping regions from collected
  /// source regions.
  SILCoverageMap *emitSourceRegions(
      SILModule &M, StringRef Name, StringRef PGOFuncName, uint64_t Hash,
      llvm::DenseMap<ProfileCounterRef, unsigned> &CounterIndices,
      SourceFile *SF, StringRef Filename) {
    if (SourceRegions.empty())
      return nullptr;

    llvm::coverage::CounterExpressionBuilder Builder;
    std::vector<SILCoverageMap::MappedRegion> Regions;
    for (const auto &Region : SourceRegions) {
      assert(Region.hasStartLoc() && "invalid region");
      assert(Region.hasEndLoc() && "incomplete region");

      auto Start = SM.getLineAndColumnInBuffer(Region.getStartLoc());
      auto End = SM.getLineAndColumnInBuffer(Region.getEndLoc());
      assert(Start.first <= End.first && "region start and end out of order");

      auto Counter = Region.getCounter(CounterMap);
      Regions.emplace_back(Start.first, Start.second, End.first, End.second,
                           Counter.expand(Builder, CounterIndices));
    }
    return SILCoverageMap::create(M, SF, Filename, Name, PGOFuncName, Hash,
                                  Regions, Builder.getExpressions());
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(D)) {
      return visitFunctionDecl(*this, AFD, [&] {
        assignKnownCounter(AFD->getBody());
      });
    } else if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
      assignKnownCounter(TLCD->getBody());
      ImplicitTopLevelBody = TLCD->getBody();
      return Action::Continue();
    }
    return Action::VisitChildrenIf(shouldWalkIntoUnhandledDecl(D));
  }

  PostWalkAction walkToDeclPost(Decl *D) override {
    if (isa<TopLevelCodeDecl>(D))
      ImplicitTopLevelBody = nullptr;
    return Action::Continue();
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (S->isImplicit() && S != ImplicitTopLevelBody)
      return Action::Continue(S);

    // If we're in an 'incomplete' region, update it to include this node. This
    // ensures we only create the region if needed.
    if (!RegionStack.empty())
      extendRegion(S);

    if (auto *BS = dyn_cast<BraceStmt>(S)) {
      if (hasCounter(BS))
        pushRegion(BS);

    } else if (auto *IS = dyn_cast<IfStmt>(S)) {
      if (auto *Cond = getConditionNode(IS->getCond()))
        assignCounter(Cond, getCurrentCounter());

      // The counter for the if statement itself tracks the number of jumps to
      // it by break statements.
      assignCounter(IS, CounterExpr::Zero());

      // We emit a counter for the then block, and define the else block in
      // terms of it.
      auto ThenCounter = assignKnownCounter(IS->getThenStmt());
      if (IS->getElseStmt()) {
        auto ElseCounter =
            CounterExpr::Sub(getCurrentCounter(), ThenCounter, CounterAlloc);
        assignCounter(IS->getElseStmt(), ElseCounter);
      }
    } else if (auto *GS = dyn_cast<GuardStmt>(S)) {
      assignCounter(GS, CounterExpr::Zero());
      assignKnownCounter(GS->getBody());

    } else if (auto *WS = dyn_cast<WhileStmt>(S)) {
      // The counter for the while statement itself tracks the number of jumps
      // to it by break and continue statements.
      assignCounter(WS, CounterExpr::Zero());

      if (auto *E = getConditionNode(WS->getCond()))
        assignCounter(E, getCurrentCounter());
      assignKnownCounter(WS->getBody());

    } else if (auto *RWS = dyn_cast<RepeatWhileStmt>(S)) {
      // The counter for the while statement itself tracks the number of jumps
      // to it by break and continue statements.
      assignCounter(RWS, CounterExpr::Zero());

      auto BodyCounter = assignKnownCounter(RWS->getBody());
      assignCounter(RWS->getCond(), BodyCounter);
      RepeatWhileStack.push_back(RWS);

    } else if (auto *FES = dyn_cast<ForEachStmt>(S)) {
      // The counter for the for statement itself tracks the number of jumps
      // to it by break and continue statements.
      assignCounter(FES, CounterExpr::Zero());
      assignKnownCounter(FES->getBody());

    } else if (auto *SS = dyn_cast<SwitchStmt>(S)) {
      // The counter for the switch statement itself tracks the number of jumps
      // to it by break statements, including the implicit breaks at the end of
      // cases.
      assignCounter(SS, CounterExpr::Zero());

      assignCounter(SS->getSubjectExpr(), getCurrentCounter());

      // Assign counters for cases so they're available for fallthrough.
      for (CaseStmt *Case : SS->getCases())
        assignKnownCounter(Case);

    } else if (auto caseStmt = dyn_cast<CaseStmt>(S)) {
      if (caseStmt->getParentKind() == CaseParentKind::Switch)
        pushRegion(S);
    } else if (auto *DS = dyn_cast<DoStmt>(S)) {
      // The counter for the do statement itself tracks the number of jumps
      // to it by break statements.
      assignCounter(DS, CounterExpr::Zero());

      assignCounter(DS->getBody(), getCurrentCounter());

    } else if (auto *DCS = dyn_cast<DoCatchStmt>(S)) {
      // The do-catch body is visited the same number of times as its parent.
      assignCounter(DCS->getBody(), getCurrentCounter());

      for (CaseStmt *Catch : DCS->getCatches())
        assignKnownCounter(Catch->getBody());

      // Initialize the exit count of the do-catch to the entry count, then
      // subtract off non-local exits as they are visited.
      assignCounter(DCS, getCurrentCounter());
      DoCatchStack.push_back(DCS);
    }
    return Action::Continue(S);
  }

  PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) override {
    if (S->isImplicit() && S != ImplicitTopLevelBody)
      return Action::Continue(S);

    if (isa<BraceStmt>(S)) {
      if (hasCounter(S)) {
        auto Adjust = setExitCount(S);
        popRegions(S);
        adjustForNonLocalExits(S, Adjust);
      }

    } else if (auto *WS = dyn_cast<WhileStmt>(S)) {
      // Update the condition with the backedge count.
      if (auto *E = getConditionNode(WS->getCond()))
        addToCounter(E, getExitCounter());

    } else if (auto *RWS = dyn_cast<RepeatWhileStmt>(S)) {
      assert(RepeatWhileStack.back() == RWS && "Malformed repeat-while stack");
      (void)RWS;
      RepeatWhileStack.pop_back();

    } else if (auto *CS = dyn_cast<ContinueStmt>(S)) {
      // Continues create extra backedges, add them to the appropriate counters.
      if (!isa<RepeatWhileStmt>(CS->getTarget()))
        addToCounter(CS->getTarget(), getCurrentCounter());
      if (auto *WS = dyn_cast<WhileStmt>(CS->getTarget())) {
        if (auto *E = getConditionNode(WS->getCond()))
          addToCounter(E, getCurrentCounter());
      }
      terminateRegion(S);

    } else if (auto *BS = dyn_cast<BreakStmt>(S)) {
      // When we break from a loop, we need to adjust the exit count.
      Stmt *BreakTarget = BS->getTarget();
      if (auto *RWS = dyn_cast<RepeatWhileStmt>(BreakTarget)) {
        subtractFromCounter(RWS->getCond(), getCurrentCounter());
      } else {
        // Update the exit counter for the target.
        addToCounter(BS->getTarget(), getCurrentCounter());
      }

      // The break also affects the exit counts of active do-catch statements.
      for (auto *DCS : DoCatchStack)
        subtractFromCounter(DCS, getCurrentCounter());

      terminateRegion(S);

    } else if (auto *FS = dyn_cast<FallthroughStmt>(S)) {
      addToCounter(FS->getFallthroughDest(), getCurrentCounter());
      terminateRegion(S);

    } else if (isa<SwitchStmt>(S)) {
      replaceCount(getCounter(S), getEndLoc(S));

    } else if (auto caseStmt = dyn_cast<CaseStmt>(S)) {
      if (caseStmt->getParentKind() == CaseParentKind::Switch) {
        // The end of a case block is an implicit break, update the exit
        // counter to reflect this.
        addToCounter(caseStmt->getParentStmt(), getCurrentCounter());
        popRegions(S);
      }
    } else if (auto *DCS = dyn_cast<DoCatchStmt>(S)) {
      assert(DoCatchStack.back() == DCS && "Malformed do-catch stack");
      DoCatchStack.pop_back();
      replaceCount(getCounter(S), getEndLoc(S));

    } else if (isa<ReturnStmt>(S) || isa<FailStmt>(S) || isa<ThrowStmt>(S)) {
      // When we return, adjust loop condition counts and do-catch exit counts
      // to reflect the early exit.
      if (isa<ReturnStmt>(S) || isa<FailStmt>(S)) {
        for (auto *RWS : RepeatWhileStack)
          subtractFromCounter(RWS->getCond(), getCurrentCounter());
        for (auto *DCS : DoCatchStack)
          subtractFromCounter(DCS, getCurrentCounter());
      }

      terminateRegion(S);
    }
    return Action::Continue(S);
  }

  PreWalkAction walkToParameterListPre(ParameterList *PL) override {
    // We don't walk into parameter lists. Default arguments should be visited
    // directly.
    // FIXME: We don't yet generate coverage for default argument generators at
    // all. This is inconsistent with property initializers, which are
    // effectively default values too. Seems like coverage doesn't offer much
    // benefit in these cases, as they're unlikely to have side effects, and
    // the values can be exercized explicitly, but we should probably at least
    // have a consistent behavior for both no matter what we choose here.
    return Action::SkipChildren();
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (shouldSkipExpr(E))
      return shouldWalkIntoExpr(E, Parent, Constant);

    // If we're in an 'incomplete' region, update it to include this node. This
    // ensures we only create the region if needed.
    if (!RegionStack.empty())
      extendRegion(E);

    // If AST visitation begins with an expression, the region stack must be
    // empty. Set up a region for the root.
    if (Parent.isNull()) {
      assert(RegionStack.empty() &&
             "Mapped a region before visiting the root?");
      assignKnownCounter(E);
    }

    if (isa<LazyInitializerExpr>(E))
      assignKnownCounter(E);

    if (hasCounter(E))
      pushRegion(E);

    assert(!RegionStack.empty() && "Must be within a region");

    if (auto *IE = dyn_cast<TernaryExpr>(E)) {
      auto ThenCounter = assignKnownCounter(IE->getThenExpr());
      auto ElseCounter =
          CounterExpr::Sub(getCurrentCounter(), ThenCounter, CounterAlloc);
      assignCounter(IE->getElseExpr(), ElseCounter);
    }
    auto WalkResult = shouldWalkIntoExpr(E, Parent, Constant);
    if (WalkResult.Action.Action == PreWalkAction::SkipChildren) {
      // We need to manually pop the region here as the ASTWalker won't call
      // the post-visitation.
      // FIXME: The ASTWalker should do a post-visit.
      if (hasCounter(E))
        popRegions(E);
    }
    return WalkResult;
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    if (shouldSkipExpr(E))
      return Action::Continue(E);

    if (hasCounter(E))
      popRegions(E);

    return Action::Continue(E);
  }
};

} // end anonymous namespace

static llvm::GlobalValue::LinkageTypes
getEquivalentPGOLinkage(FormalLinkage Linkage) {
  switch (Linkage) {
  case FormalLinkage::PublicUnique:
  case FormalLinkage::PublicNonUnique:
    return llvm::GlobalValue::ExternalLinkage;

  case FormalLinkage::HiddenUnique:
  case FormalLinkage::Private:
    return llvm::GlobalValue::PrivateLinkage;
  }

  llvm_unreachable("Unhandled FormalLinkage in switch.");
}

static void walkNode(NodeToProfile Node, ASTWalker &Walker) {
  if (auto N = Node.getAsNode()) {
    N.walk(Walker);
  } else {
    // We want to walk the SourceFile for a top-level entry point. We will only
    // assign regions to TopLevelCodeDecls.
    Node.getAsSourceFile()->walk(Walker);
  }
}

void SILProfiler::assignRegionCounters() {
  const auto &SM = M.getASTContext().SourceMgr;
  auto *DC = forDecl.getInnermostDeclContext();
  auto *SF = DC->getParentSourceFile();
  assert(SF && "Not within a SourceFile?");

  CurrentFileName = SF->getFilename();

  MapRegionCounters Mapper(forDecl, RegionCounterMap);

  auto Root = getNodeToProfile(forDecl);

  auto CurrentFuncName = forDecl.mangle();
  auto CurrentFuncLinkage = FormalLinkage::HiddenUnique;

  if (auto N = Root.getAsNode()) {
    if (auto *D = N.dyn_cast<Decl *>()) {
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(D))
        CurrentFuncLinkage = getDeclLinkage(AFD);
    }
  }

  PGOFuncName = llvm::getPGOFuncName(
      CurrentFuncName, getEquivalentPGOLinkage(CurrentFuncLinkage),
      CurrentFileName);

  assert((!CurrentFuncName.empty() && !PGOFuncName.empty()) &&
         "Expected covered region to be named");

  LLVM_DEBUG(llvm::dbgs() << "Assigning counters to: " << CurrentFuncName
                          << "\n");
  walkNode(Root, Mapper);

  NumRegionCounters = Mapper.NextCounter;
  // TODO: Mapper needs to calculate a function hash as it goes.
  PGOFuncHash = 0x0;

  if (EmitCoverageMapping) {
    CoverageMapping Coverage(SM, forDecl);
    walkNode(Root, Coverage);
    CovMap =
        Coverage.emitSourceRegions(M, CurrentFuncName, PGOFuncName, PGOFuncHash,
                                   RegionCounterMap, SF, CurrentFileName);
  }

  if (llvm::IndexedInstrProfReader *IPR = M.getPGOReader()) {
    auto LoadedCounts = IPR->getInstrProfRecord(PGOFuncName, PGOFuncHash);
    if (auto E = LoadedCounts.takeError()) {
      llvm::handleAllErrors(std::move(E), [](const llvm::InstrProfError &Err) {
        Err.log(llvm::dbgs());
        return;
      });
      llvm::dbgs() << PGOFuncName << "\n";
      return;
    }
    PGOMapping pgoMapper(forDecl, RegionCounterMap, LoadedCounts.get(),
                         RegionLoadedCounterMap, RegionCondToParentMap);
    walkNode(Root, pgoMapper);
  }
}

ProfileCounter SILProfiler::getExecutionCount(ProfileCounterRef Ref) {
  if (!M.getPGOReader() || !hasRegionCounters())
    return ProfileCounter();

  auto it = RegionLoadedCounterMap.find(Ref);
  if (it == RegionLoadedCounterMap.end()) {
    return ProfileCounter();
  }
  return it->getSecond();
}

ProfileCounter SILProfiler::getExecutionCount(ASTNode Node) {
  return getExecutionCount(ProfileCounterRef::node(Node));
}

Optional<ASTNode> SILProfiler::getPGOParent(ASTNode Node) {
  if (!Node || !M.getPGOReader() || !hasRegionCounters()) {
    return None;
  }
  auto it = RegionCondToParentMap.find(Node);
  if (it == RegionCondToParentMap.end()) {
    return None;
  }
  return it->getSecond();
}
