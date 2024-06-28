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
#include "swift/Basic/Assertions.h"
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

  if (auto *D = DC->getInnermostDeclarationDeclContext()) {
    // Do not profile AST nodes in unavailable contexts.
    if (D->getSemanticUnavailableAttr()) {
      LLVM_DEBUG(llvm::dbgs() << "Skipping ASTNode: unavailable context\n");
      return false;
    }

    // Do not profile functions that have had their bodies replaced (e.g
    // function body macros).
    // TODO: If/when preamble macros become an official feature, we'll
    // need to be more nuanced here.
    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(D)) {
      if (AFD->getOriginalBodySourceRange() != AFD->getBodySourceRange()) {
        LLVM_DEBUG(llvm::dbgs() << "Skipping function: body replaced\n");
        return false;
      }
    }
  }

  // Do not profile code that hasn't been written by the user.
  if (!Constant.hasUserWrittenCode()) {
    LLVM_DEBUG(llvm::dbgs() << "Skipping ASTNode: no user-written code\n");
    return false;
  }

  return true;
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
  case Kind::Node:
    break;
  case Kind::ErrorBranch:
    OS << "error branch of: ";
    break;
  }
  switch (RefKind) {
  case Kind::Node:
  case Kind::ErrorBranch: {
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
    break;
  case Kind::ErrorBranch:
    OS << "error branch of:\n";
    Node.dump(OS.indent(2));
    break;
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
  return ASTWalker::Action::SkipNode();
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
  // We want to walk into initializers for bindings, and the expansions of
  // MacroExpansionDecls, which will be nested within MacroExpansionExprs in
  // local contexts. We won't record any regions within the macro expansion,
  // but still need to walk to get accurate counter information in case e.g
  // there's a throwing function call in the expansion.
  return isa<PatternBindingDecl>(D) || isa<MacroExpansionDecl>(D);
}

/// Whether the expression \c E could potentially throw an error.
static bool mayExpressionThrow(const Expr *E) {
  if (auto *AE = dyn_cast<ApplyExpr>(E)) {
    // Throws if the function throws.
    return bool(AE->throws());
  }
  if (auto *S = dyn_cast<SubscriptExpr>(E)) {
    // Throws if subscript has a throwing getter.
    auto *SD = cast<SubscriptDecl>(S->getDecl().getDecl());
    if (auto *accessor = SD->getEffectfulGetAccessor())
      return accessor->hasThrows();
  }
  if (auto *DE = dyn_cast<DeclRefExpr>(E)) {
    if (auto *VD = dyn_cast<VarDecl>(DE->getDecl())) {
      // Throws if the getter throws.
      if (auto *accessor = VD->getEffectfulGetAccessor())
        return accessor->hasThrows();
    }
  }
  return false;
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

  bool shouldWalkIntoPropertyWrapperPlaceholderValue() override {
    // Don't walk into PropertyWrapperValuePlaceholderExprs, these should be
    // mapped as part of the wrapped value initialization.
    return false;
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
    return Action::VisitNodeIf(shouldWalkIntoUnhandledDecl(D));
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
      mapRegion(CS);
    }
    return Action::Continue(S);
  }

  PreWalkAction walkToParameterListPre(ParameterList *PL) override {
    // We don't walk into parameter lists. Default arguments should be visited
    // directly.
    // FIXME: We don't yet profile default argument generators at all.
    return Action::SkipNode();
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

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    if (shouldSkipExpr(E))
      return Action::Continue(E);

    // If we have an expr that may throw an error, give it a counter for the
    // error branch.
    if (mayExpressionThrow(E))
      mapRegion(ProfileCounterRef::errorBranchOf(E));

    return Action::Continue(E);
  }
};

class CounterExpr {
  llvm::coverage::Counter Counter;

  explicit CounterExpr(llvm::coverage::Counter Counter) : Counter(Counter) {}

public:
  static CounterExpr Concrete(unsigned Idx) {
    return CounterExpr(llvm::coverage::Counter::getCounter(Idx));
  }
  static CounterExpr Zero() {
    return CounterExpr(llvm::coverage::Counter::getZero());
  }

  static CounterExpr Add(CounterExpr LHS, CounterExpr RHS,
                         llvm::coverage::CounterExpressionBuilder &Builder) {
    return CounterExpr(Builder.add(LHS.getLLVMCounter(), RHS.getLLVMCounter()));
  }
  static CounterExpr Sub(CounterExpr LHS, CounterExpr RHS,
                         llvm::coverage::CounterExpressionBuilder &Builder) {
    return CounterExpr(
        Builder.subtract(LHS.getLLVMCounter(), RHS.getLLVMCounter()));
  }

  /// Returns true if this is a zero counter.
  bool isZero() const { return Counter.isZero(); }

  friend bool operator==(const CounterExpr &LHS, const CounterExpr &RHS) {
    return LHS.Counter == RHS.Counter;
  }
  friend bool operator!=(const CounterExpr &LHS, const CounterExpr &RHS) {
    return !(LHS == RHS);
  }

  llvm::coverage::Counter getLLVMCounter() const { return Counter; }

  void print(raw_ostream &OS,
             const llvm::coverage::CounterExpressionBuilder &Builder) const {
    SILCoverageMap::printCounter(OS, Counter, Builder.getExpressions());
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD
  void dump(const llvm::coverage::CounterExpressionBuilder &Builder) const {
    print(llvm::errs(), Builder);
  }
#endif
};

/// A region of source code that can be mapped to a counter.
class SourceMappingRegion {
public:
  enum class Kind {
    /// A region that is associated with an ASTNode, and defines a scope under
    /// which the region is active.
    Node,

    /// A node region that is only present for scoping of child regions, and
    /// doesn't need to be included in the resulting set of regions.
    ScopingOnly,

    /// A region that refines the counter of a node region. This doesn't have
    /// an ASTNode of its own.
    Refined,
  };

private:
  Kind RegionKind;
  ASTNode Node;

  /// The counter for an incomplete region. Note we do not store counters
  /// for nodes, as we need to be able to fix them up after popping the regions.
  std::optional<CounterExpr> Counter;

  /// The region's starting location.
  std::optional<SourceLoc> StartLoc;

  /// The region's ending location.
  std::optional<SourceLoc> EndLoc;

  /// Whether the region is within a macro expansion. Such regions do not
  /// get recorded, but are needed to track the counters within the expansion.
  bool IsInMacroExpansion = false;

  SourceMappingRegion(Kind RegionKind, std::optional<CounterExpr> Counter,
                      std::optional<SourceLoc> StartLoc)
      : RegionKind(RegionKind), Counter(Counter), StartLoc(StartLoc) {
    assert((!StartLoc || StartLoc->isValid()) &&
           "Expected start location to be valid");
  }

  SourceMappingRegion(Kind RegionKind, ASTNode Node, SourceRange Range,
                      std::optional<CounterExpr> Counter,
                      const SourceManager &SM)
      : RegionKind(RegionKind), Node(Node), Counter(Counter) {
    assert(Range.isValid());
    StartLoc = Range.Start;
    EndLoc = Lexer::getLocForEndOfToken(SM, Range.End);
  }

public:
  /// Create a regular source region for an ASTNode.
  static SourceMappingRegion forNode(ASTNode Node, const SourceManager &SM,
                                     SourceRange Range = SourceRange()) {
    if (Range.isInvalid())
      Range = Node.getSourceRange();

    // Note we don't store counters for nodes, as we need to be able to fix them
    // up later.
    return SourceMappingRegion(Kind::Node, Node, Range,
                               /*Counter*/ std::nullopt, SM);
  }

  /// Create a source region for an ASTNode that is only present for scoping of
  /// child regions, and doesn't need to be included in the resulting set of
  /// regions.
  static SourceMappingRegion
  scopingOnly(ASTNode Node, const SourceManager &SM,
              std::optional<CounterExpr> Counter = std::nullopt) {
    return SourceMappingRegion(Kind::ScopingOnly, Node, Node.getSourceRange(),
                               Counter, SM);
  }

  /// Create a refined region for a given counter.
  static SourceMappingRegion refined(CounterExpr Counter,
                                     std::optional<SourceLoc> StartLoc) {
    return SourceMappingRegion(Kind::Refined, Counter, StartLoc);
  }

  SourceMappingRegion(SourceMappingRegion &&Region) = default;
  SourceMappingRegion &operator=(SourceMappingRegion &&RHS) = default;

  bool isInMacroExpansion() const {
    return IsInMacroExpansion;
  }

  void setIsInMacroExpansion() {
    IsInMacroExpansion = true;
  }

  /// Whether this region is for scoping only.
  bool isForScopingOnly() const { return RegionKind == Kind::ScopingOnly; }

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

  /// Whether the region has a non-empty range.
  bool hasNonEmptyRange() const {
    return StartLoc && EndLoc && *StartLoc != *EndLoc;
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
    return Action::VisitNodeIf(shouldWalkIntoUnhandledDecl(D));
  }

  LazyInitializerWalking getLazyInitializerWalkingBehavior() override {
    // We want to walk lazy initializers present in the synthesized getter for
    // a lazy variable.
    return LazyInitializerWalking::InAccessor;
  }

  bool shouldWalkIntoPropertyWrapperPlaceholderValue() override {
    // Don't walk into PropertyWrapperValuePlaceholderExprs, these should be
    // mapped as part of the wrapped value initialization.
    return false;
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
      setKnownExecutionCount(CS);
    }
    return Action::Continue(S);
  }

  PreWalkAction walkToParameterListPre(ParameterList *PL) override {
    // We don't walk into parameter lists. Default arguments should be visited
    // directly.
    // FIXME: We don't yet profile default argument generators at all.
    return Action::SkipNode();
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

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    if (shouldSkipExpr(E))
      return Action::Continue(E);

    // If we have an expr that may throw an error, give it a counter for the
    // error branch.
    if (mayExpressionThrow(E))
      setKnownExecutionCount(ProfileCounterRef::errorBranchOf(E));

    return Action::Continue(E);
  }
};

/// Produce coverage mapping information for a function. This involves taking
/// the counters computed by MapRegionCounters, and annotating the source with
/// regions that are defined in terms of those counters.
struct CoverageMapping : public ASTWalker {
private:
  const SourceManager &SM;
  SourceFile *SF;

  /// The SIL function being profiled.
  SILDeclRef Constant;

  /// Builder needed to produce CounterExprs.
  llvm::coverage::CounterExpressionBuilder CounterBuilder;

  /// The map of statements to counter expressions.
  llvm::DenseMap<ProfileCounterRef, CounterExpr> CounterExprs;

  /// The map of counter references to their concrete counter indices.
  const llvm::DenseMap<ProfileCounterRef, unsigned> &ConcreteCounters;

  /// The source mapping regions for this function.
  std::vector<SourceMappingRegion> SourceRegions;

  /// A stack of currently live regions.
  std::vector<SourceMappingRegion> RegionStack;

  /// A stack of active repeat-while loops.
  std::vector<RepeatWhileStmt *> RepeatWhileStack;

  std::optional<CounterExpr> ExitCounter;

  Stmt *ImplicitTopLevelBody = nullptr;

  /// The number of parent MacroExpansionExprs.
  unsigned MacroDepth = 0;

  /// Whether the current walk is within a macro expansion.
  bool isInMacroExpansion() const { return MacroDepth > 0; }

  /// Return true if \c Ref has an associated counter.
  bool hasCounter(ProfileCounterRef Ref) { return CounterExprs.count(Ref); }

  /// Return true if \c Node has an associated counter.
  bool hasCounter(ASTNode Node) {
    return hasCounter(ProfileCounterRef::node(Node));
  }

  /// Return the region counter for \c Ref.
  ///
  /// This should only be called on references that have a dedicated counter.
  CounterExpr getCounter(ProfileCounterRef Ref) {
    auto Iter = CounterExprs.find(Ref);
    assert(Iter != CounterExprs.end() && "No counter found");
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
    auto Res = CounterExprs.insert({Ref, Expr});

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
    auto Iter = ConcreteCounters.find(Ref);
    assert(Iter != ConcreteCounters.end() && "Should have mapped this counter");
    auto Counter = CounterExpr::Concrete(Iter->second);
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
      Counter = CounterExpr::Add(Counter, std::move(Expr), CounterBuilder);
    }
    assignCounter(Node, Counter);
  }

  /// Subtract \c Expr from \c Node's counter.
  void subtractFromCounter(ASTNode Node, CounterExpr Expr) {
    if (Expr.isZero())
      return;

    auto Counter = getCounter(Node);
    assert(!Counter.isZero() && "Cannot create a negative counter");
    assignCounter(Node,
                  CounterExpr::Sub(Counter, std::move(Expr), CounterBuilder));
  }

  /// Return the current region's counter.
  CounterExpr getCurrentCounter() {
    return getRegion().getCounter(CounterExprs);
  }

  /// Get the counter from the end of the most recent scope.
  CounterExpr getExitCounter() {
    assert(ExitCounter && "no exit counter available");
    return *ExitCounter;
  }

  /// Set the exit count so we can leave the scope related to \c Node
  ///
  /// Returns the delta of the count on entering \c Node and exiting, or null if
  /// there was no change.
  std::optional<CounterExpr> setExitCount(ASTNode Node) {
    // A `try?` absorbs child error branches, so we can assume the exit count is
    // the same as the entry count in that case.
    // NOTE: This assumes there is no other kind of control flow that can happen
    // in a nested expression, which is true today, but may not always be.
    if (Node.isExpr(ExprKind::OptionalTry))
      return std::nullopt;

    ExitCounter = getCurrentCounter();
    if (hasCounter(Node) && getRegion().getNode() != Node)
      return CounterExpr::Sub(getCounter(Node), *ExitCounter, CounterBuilder);
    return std::nullopt;
  }

  /// Adjust the count for control flow when exiting a scope.
  void adjustForNonLocalExits(ASTNode Scope,
                              std::optional<CounterExpr> ControlFlowAdjust) {
    // If there are no regions left, there's nothing to adjust.
    if (RegionStack.empty())
      return;

    // If the region is for a brace, check to see if we have a parent labeled
    // statement, in which case the exit count needs to account for any direct
    // jumps to it though e.g break statements.
    std::optional<CounterExpr> JumpsToLabel;
    if (Scope.isStmt(StmtKind::Brace)) {
      if (auto *ParentStmt = Parent.getAsStmt()) {
        if (auto *DCS = dyn_cast<DoCatchStmt>(ParentStmt)) {
          // We need to handle the brace of a DoCatchStmt here specially,
          // applying the same logic we apply to the catch clauses (handled by
          // the CaseStmt logic), we add on the exit count of the branch to the
          // statement's exit count.
          addToCounter(DCS, getExitCounter());
          return;
        }

        // Don't apply exit adjustments to if statement branches, they should
        // be handled at the end of the statement. This avoids creating awkward
        // overlapping exit regions for each branch, and ensures 'break'
        // statements only have their jump counted once for the entire
        // statement.
        if (isa<IfStmt>(ParentStmt))
          return;

        if (auto *LS = dyn_cast<LabeledStmt>(ParentStmt))
          JumpsToLabel = getCounter(LS);
      }
    }

    if (!ControlFlowAdjust && !JumpsToLabel)
      return;

    auto Count = getCurrentCounter();
    // Add the counts from jumps directly to the label (such as breaks)
    if (JumpsToLabel)
      Count = CounterExpr::Add(Count, *JumpsToLabel, CounterBuilder);
    // Now apply any adjustments for control flow.
    if (ControlFlowAdjust)
      Count = CounterExpr::Sub(Count, *ControlFlowAdjust, CounterBuilder);

    replaceCount(Count, getEndLoc(Scope));
  }

  /// Push a region onto the stack.
  void pushRegion(SourceMappingRegion Region) {
    // Note on the region whether we're currently in a macro expansion.
    if (isInMacroExpansion())
      Region.setIsInMacroExpansion();

    LLVM_DEBUG({
      llvm::dbgs() << "Pushed region: ";
      Region.print(llvm::dbgs(), SM);
      llvm::dbgs() << "\n";
    });
    RegionStack.push_back(std::move(Region));
  }

  /// Replace the current region at \p Start with a new counter. If \p Start is
  /// \c None, or the counter is semantically zero, an 'incomplete' region is
  /// formed, which is not recorded unless followed by additional AST nodes.
  void replaceCount(CounterExpr Counter, std::optional<SourceLoc> Start) {
    // If the counter is zero, form an 'incomplete' region with no starting
    // location. This prevents forming unreachable regions unless there is a
    // following statement or expression to extend the region.
    if (Start && Counter.isZero())
      Start = std::nullopt;

    pushRegion(SourceMappingRegion::refined(Counter, Start));
  }

  /// Get the location for the end of the last token in \c Node.
  SourceLoc getEndLoc(ASTNode Node) {
    return Lexer::getLocForEndOfToken(SM, Node.getEndLoc());
  }

  /// Record a popped region in the resulting list of regions.
  void takePoppedRegion(SourceMappingRegion &&Region, SourceLoc ParentEndLoc) {
    LLVM_DEBUG({
      llvm::dbgs() << "Popped region: ";
      Region.print(llvm::dbgs(), SM);
      llvm::dbgs() << "\n";
    });

    // Don't record regions in macro expansions, they don't have source
    // locations that can be meaningfully mapped to source code.
    if (Region.isInMacroExpansion())
      return;

    // Don't bother recording regions that are only present for scoping.
    if (Region.isForScopingOnly())
      return;

    // Don't record incomplete regions.
    if (!Region.hasStartLoc())
      return;

    // Set the region end location to the end location of the parent.
    if (!Region.hasEndLoc())
      Region.setEndLoc(ParentEndLoc);

    // If the range ended up being empty, ignore it (this can happen when we
    // replace the counter, and don't extend the region any further).
    if (!Region.hasNonEmptyRange())
      return;

    SourceRegions.push_back(std::move(Region));
  }

  /// Pop regions from the stack into the function's list of regions.
  ///
  /// Adds all regions from \c ParentNode to the top of the stack to the
  /// function's \c SourceRegions.
  void popRegions(ASTNode ParentNode) {
    auto I = llvm::find_if(RegionStack, [&](const SourceMappingRegion &Region) {
      return Region.getNode().getOpaqueValue() == ParentNode.getOpaqueValue();
    });
    auto E = RegionStack.end();
    assert(I != E && "parent not in stack");
    assert(I->hasNonEmptyRange() && "Pushed node with empty range?");

    auto EndLoc = I->getEndLoc();
    for (auto &Region : llvm::make_range(I, E))
      takePoppedRegion(std::move(Region), EndLoc);

    RegionStack.erase(I, E);
  }

  /// Exit the given region, popping it and its children from the region stack,
  /// and adjusting the following counter if needed.
  void exitRegion(ASTNode Node) {
    auto Adjust = setExitCount(Node);
    popRegions(Node);
    adjustForNonLocalExits(Node, Adjust);
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
    assert(!RegionStack.empty() && "Cannot terminate non-existant region");

    // Walk up the region stack and cut short regions until we reach a region
    // for an AST node. This ensures we correctly handle new regions that have
    // been introduced as a result of replacing the count, e.g if errors have
    // been thrown.
    for (auto &Region : llvm::reverse(RegionStack)) {
      if (!Region.hasEndLoc())
        Region.setEndLoc(getEndLoc(S));
      if (Region.getNode())
        break;
    }
    replaceCount(CounterExpr::Zero(), /*Start*/ std::nullopt);
  }

  Expr *getConditionNode(StmtCondition SC) {
    assert(!SC.empty() && "Empty condition");
    return SC.front().getBooleanOrNull();
  }

public:
  CoverageMapping(
      SourceFile *SF, SILDeclRef Constant,
      const llvm::DenseMap<ProfileCounterRef, unsigned> &ConcreteCounters)
    : SM(SF->getASTContext().SourceMgr), SF(SF), Constant(Constant),
      ConcreteCounters(ConcreteCounters) {}

  LazyInitializerWalking getLazyInitializerWalkingBehavior() override {
    // We want to walk lazy initializers present in the synthesized getter for
    // a lazy variable.
    return LazyInitializerWalking::InAccessor;
  }

  bool shouldWalkIntoPropertyWrapperPlaceholderValue() override {
    // Don't walk into PropertyWrapperValuePlaceholderExprs, these should be
    // mapped as part of the wrapped value initialization.
    return false;
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  /// Generate the coverage counter mapping regions from collected
  /// source regions.
  SILCoverageMap *emitSourceRegions(SILModule &M, StringRef Name,
                                    StringRef PGOFuncName, uint64_t Hash,
                                    StringRef Filename) {
    if (SourceRegions.empty())
      return nullptr;

    auto FileSourceRange = SM.getRangeForBuffer(*SF->getBufferID());
    auto isLocInFile = [&](SourceLoc Loc) {
      return FileSourceRange.contains(Loc) || FileSourceRange.getEnd() == Loc;
    };

    using MappedRegion = SILCoverageMap::MappedRegion;
    std::vector<MappedRegion> Regions;
    SourceRange OuterRange;
    for (const auto &Region : SourceRegions) {
      assert(Region.hasStartLoc() && "invalid region");
      assert(Region.hasEndLoc() && "incomplete region");

      SourceRange Range(Region.getStartLoc(), Region.getEndLoc());

      // Make sure we haven't ended up with any source locations outside the
      // SourceFile (e.g for generated code such as macros), asserting in an
      // asserts build, dropping in a non-asserts build.
      if (!isLocInFile(Range.Start) || !isLocInFile(Range.End)) {
        assert(false && "range outside of file");
        continue;
      }

      // Build up the outer range from the union of all coverage regions.
      if (!OuterRange) {
        OuterRange = Range;
      } else {
        OuterRange.widen(Range);
      }

      auto Start = SM.getLineAndColumnInBuffer(Region.getStartLoc());
      auto End = SM.getLineAndColumnInBuffer(Region.getEndLoc());
      assert(Start.first <= End.first && "region start and end out of order");

      auto Counter = Region.getCounter(CounterExprs);
      Regions.push_back(MappedRegion::code(Start.first, Start.second, End.first,
                                           End.second,
                                           Counter.getLLVMCounter()));
    }
    // Add any skipped regions present in the outer range.
    for (auto clause : SF->getIfConfigClausesWithin(OuterRange)) {
      CharSourceRange SkipRange;
      switch (clause.getKind()) {
      case IfConfigClauseRangeInfo::ActiveClause:
      case IfConfigClauseRangeInfo::EndDirective:
        SkipRange = clause.getDirectiveRange(SM);
        break;
      case IfConfigClauseRangeInfo::InactiveClause:
        SkipRange = clause.getWholeRange(SM);
        break;
      }
      if (SkipRange.getByteLength() == 0)
        continue;

      auto Start = SM.getLineAndColumnInBuffer(SkipRange.getStart());
      auto End = SM.getLineAndColumnInBuffer(SkipRange.getEnd());
      assert(Start.first <= End.first && "region start and end out of order");

      // If this is consecutive with the last one, expand it.
      if (!Regions.empty()) {
        auto &last = Regions.back();
        if (last.RegionKind == MappedRegion::Kind::Skipped &&
            last.EndLine == Start.first && last.EndCol == Start.second) {
          last.EndLine = End.first;
          last.EndCol = End.second;
          continue;
        }
      }

      Regions.push_back(MappedRegion::skipped(Start.first, Start.second,
                                              End.first, End.second));
    }
    return SILCoverageMap::create(M, SF, Filename, Name, PGOFuncName, Hash,
                                  Regions, CounterBuilder.getExpressions());
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
    return Action::VisitNodeIf(shouldWalkIntoUnhandledDecl(D));
  }

  PostWalkAction walkToDeclPost(Decl *D) override {
    if (isa<TopLevelCodeDecl>(D))
      ImplicitTopLevelBody = nullptr;
    return Action::Continue();
  }

  class SetParentRAII final {
    ASTWalker &Walker;
    decltype(ASTWalker::Parent) PriorParent;

  public:
    template <typename T>
    SetParentRAII(ASTWalker &walker, T *newParent)
        : Walker(walker), PriorParent(walker.Parent) {
      walker.Parent = newParent;
    }

    ~SetParentRAII() { Walker.Parent = PriorParent; }
  };

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (S->isImplicit() && S != ImplicitTopLevelBody)
      return Action::Continue(S);

    // If we're in an 'incomplete' region, update it to include this node. This
    // ensures we only create the region if needed.
    if (!RegionStack.empty())
      extendRegion(S);

    if (auto *BS = dyn_cast<BraceStmt>(S)) {
      if (hasCounter(BS))
        pushRegion(SourceMappingRegion::forNode(BS, SM));

    } else if (auto *IS = dyn_cast<IfStmt>(S)) {
      // The counter for the if statement itself tracks the number of jumps to
      // it by break statements.
      assignCounter(IS, CounterExpr::Zero());

      // FIXME: This is a redundant region for non else-ifs.
      if (auto *Cond = getConditionNode(IS->getCond()))
        assignCounter(Cond, getCurrentCounter());

      // Visit the children.
      // FIXME: This is a hack.
      {
        SetParentRAII R(*this, S);
        for (auto Cond : IS->getCond())
          Cond.walk(*this);

        // The parent counter is taken after the condition in case e.g
        // it threw an error.
        auto ParentCounter = getCurrentCounter();

        // We emit a counter for the then block, and define the else block in
        // terms of it.
        auto ThenCounter = assignKnownCounter(IS->getThenStmt());
        IS->getThenStmt()->walk(*this);
        auto ThenDelta =
            CounterExpr::Sub(ThenCounter, getExitCounter(), CounterBuilder);

        std::optional<CounterExpr> ElseDelta;
        if (auto *Else = IS->getElseStmt()) {
          auto ElseCounter = CounterExpr::Sub(ParentCounter, ThenCounter,
                                              CounterBuilder);
          // We handle `else if` and `else` slightly differently here. For
          // `else` we have a BraceStmt, and can use the existing scoping logic
          // to handle calculating the exit count. For `else if`, we need to
          // set up a new scope to contain the child `if` statement, effectively
          // we treat:
          //
          // if .random() {
          // } else if .random() {
          // } else {
          // }
          //
          // the same as:
          //
          // if .random() {
          // } else {
          //   if .random() {
          //   } else {
          //   }
          // }
          //
          // This ensures we assign a correct counter to the `else if`
          // condition, and allows us to compute the exit count correctly. We
          // don't need the fake `else` scope to be included in the resulting
          // set of regions, so we mark it scoping-only.
          if (isa<BraceStmt>(Else)) {
            assignCounter(Else, ElseCounter);
          } else {
            pushRegion(SourceMappingRegion::scopingOnly(Else, SM, ElseCounter));
          }
          Else->walk(*this);

          // Once we've walked the `else`, compute the delta exit count. For
          // a normal `else` we can use the computed exit count, for an
          // `else if` we can take the current region count since we don't have
          // a proper scope. This is a little hacked together, but we'll be able
          // to do away with all of this once we re-implement as a SILOptimizer
          // pass.
          auto AfterElse = isa<BraceStmt>(Else) ? getExitCounter()
                                                : getCurrentCounter();
          if (!isa<BraceStmt>(Else))
            popRegions(Else);

          ElseDelta = CounterExpr::Sub(ElseCounter, AfterElse, CounterBuilder);
        }
        // Compute the exit count following the `if`, taking jumps to the
        // statement by breaks into account, and the delta of the `then` branch
        // and `else` branch if we have one.
        auto AfterIf = getCurrentCounter();
        AfterIf = CounterExpr::Add(AfterIf, getCounter(IS), CounterBuilder);
        AfterIf = CounterExpr::Sub(AfterIf, ThenDelta, CounterBuilder);
        if (ElseDelta)
          AfterIf = CounterExpr::Sub(AfterIf, *ElseDelta, CounterBuilder);

        if (AfterIf != getCurrentCounter())
          replaceCount(AfterIf, getEndLoc(IS));
      }
      // Already visited the children.
      return Action::SkipChildren(S);

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

      // FIXME: This is a redundant region.
      assignCounter(SS->getSubjectExpr(), getCurrentCounter());

      // Assign counters for cases so they're available for fallthrough.
      for (CaseStmt *Case : SS->getCases())
        assignKnownCounter(Case);

    } else if (auto *DCS = dyn_cast<DoCatchStmt>(S)) {
      // The counter for the do-catch statement itself tracks the number of
      // jumps to it by break statements, including the implicit breaks at the
      // end of body + catches.
      assignCounter(DCS, CounterExpr::Zero());

      // The do-catch body is visited the same number of times as its parent.
      assignCounter(DCS->getBody(), getCurrentCounter());

      // The catch clauses are CaseStmts that have their own mapped counters.
      for (CaseStmt *Catch : DCS->getCatches())
        assignKnownCounter(Catch);

    } else if (auto *DS = dyn_cast<DoStmt>(S)) {
      // The counter for the do statement itself tracks the number of jumps
      // to it by break statements.
      assignCounter(DS, CounterExpr::Zero());

      // The do body is visited the same number of times as its parent.
      assignCounter(DS->getBody(), getCurrentCounter());

    } else if (auto *CS = dyn_cast<CaseStmt>(S)) {
      SourceRange Range;
      switch (CS->getParentKind()) {
      case CaseParentKind::DoCatch:
        // For a catch clause, we only want the range to cover the brace.
        Range = CS->getBody()->getSourceRange();
        break;
      case CaseParentKind::Switch:
        // FIXME: We may want to reconsider using the full range here, as it
        // implies the case pattern is evaluated the same number of times as
        // the body, which is not true. We don't currently have a way of
        // tracking the pattern evaluation count though.
        Range = CS->getSourceRange();
        break;
      }
      pushRegion(SourceMappingRegion::forNode(CS, SM, Range));
    }
    return Action::Continue(S);
  }

  PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) override {
    if (S->isImplicit() && S != ImplicitTopLevelBody)
      return Action::Continue(S);

    if (isa<BraceStmt>(S)) {
      if (hasCounter(S))
        exitRegion(S);

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

      terminateRegion(S);

    } else if (auto *FS = dyn_cast<FallthroughStmt>(S)) {
      addToCounter(FS->getFallthroughDest(), getCurrentCounter());
      terminateRegion(S);

    } else if (isa<SwitchStmt>(S) || isa<DoCatchStmt>(S)) {
      // Replace the parent counter with the exit count of the statement.
      replaceCount(getCounter(S), getEndLoc(S));

    } else if (auto *CS = dyn_cast<CaseStmt>(S)) {
      // The end of a case/catch block is an implicit break, update the exit
      // counter to reflect this.
      addToCounter(CS->getParentStmt(), getCurrentCounter());
      popRegions(S);

    } else if (isa<ReturnStmt>(S) || isa<FailStmt>(S) || isa<ThrowStmt>(S)) {
      // When we return, adjust loop condition counts and do-catch exit counts
      // to reflect the early exit.
      if (isa<ReturnStmt>(S) || isa<FailStmt>(S)) {
        for (auto *RWS : RepeatWhileStack)
          subtractFromCounter(RWS->getCond(), getCurrentCounter());
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
    return Action::SkipNode();
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

    if (hasCounter(E)) {
      pushRegion(SourceMappingRegion::forNode(E, SM));
    } else if (isa<OptionalTryExpr>(E) || isa<MacroExpansionExpr>(E)) {
      // If we have a `try?`, that doesn't already have a counter, record it
      // as a scoping-only region. We need it to scope child error branches,
      // but don't need it in the resulting set of regions.
      //
      // If we have a macro expansion, also push a scoping-only region. We'll
      // discard any regions recorded within the macro, but will adjust for any
      // control flow that may have happened within the macro.
      assignCounter(E, getCurrentCounter());
      pushRegion(SourceMappingRegion::scopingOnly(E, SM));
    }

    assert(!RegionStack.empty() && "Must be within a region");

    if (auto *TE = dyn_cast<TernaryExpr>(E)) {
      assert(shouldWalkIntoExpr(TE, Parent, Constant).Action.Action ==
                 PreWalkAction::Continue &&
             "Currently this only returns false for closures");

      // Visit the children.
      // FIXME: This is a hack.
      {
        SetParentRAII R(*this, TE);
        TE->getCondExpr()->walk(*this);

        // The parent counter is taken after the condition in case e.g
        // it threw an error.
        auto ParentCounter = getCurrentCounter();

        auto *Then = TE->getThenExpr();
        auto ThenCounter = assignKnownCounter(Then);
        Then->walk(*this);

        auto *Else = TE->getElseExpr();
        auto ElseCounter =
            CounterExpr::Sub(ParentCounter, ThenCounter, CounterBuilder);
        assignCounter(Else, ElseCounter);
        Else->walk(*this);
      }
      // Already visited the children.
      return Action::SkipChildren(TE);
    }

    if (isa<MacroExpansionExpr>(E))
      MacroDepth += 1;

    return shouldWalkIntoExpr(E, Parent, Constant);
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    if (shouldSkipExpr(E))
      return Action::Continue(E);

    // The region following the expression gets current counter minus the error
    // branch counter, i.e the number of times we didn't throw an error.
    if (mayExpressionThrow(E)) {
      auto ThrowCount = assignKnownCounter(ProfileCounterRef::errorBranchOf(E));
      replaceCount(
          CounterExpr::Sub(getCurrentCounter(), ThrowCount, CounterBuilder),
          Lexer::getLocForEndOfToken(SM, E->getEndLoc()));
    }

    if (isa<MacroExpansionExpr>(E)) {
      assert(isInMacroExpansion());
      MacroDepth -= 1;
    }

    if (hasCounter(E))
      exitRegion(E);

    return Action::Continue(E);
  }
};

} // end anonymous namespace

static llvm::GlobalValue::LinkageTypes
getEquivalentPGOLinkage(FormalLinkage Linkage) {
  switch (Linkage) {
  case FormalLinkage::PublicUnique:
  case FormalLinkage::PublicNonUnique:
  case FormalLinkage::PackageUnique:
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
    CoverageMapping Coverage(SF, forDecl, RegionCounterMap);
    walkNode(Root, Coverage);
    CovMap = Coverage.emitSourceRegions(M, CurrentFuncName, PGOFuncName,
                                        PGOFuncHash, CurrentFileName);
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

std::optional<ASTNode> SILProfiler::getPGOParent(ASTNode Node) {
  if (!Node || !M.getPGOReader() || !hasRegionCounters()) {
    return std::nullopt;
  }
  auto it = RegionCondToParentMap.find(Node);
  if (it == RegionCondToParentMap.end()) {
    return std::nullopt;
  }
  return it->getSecond();
}

unsigned SILProfiler::getCounterIndexFor(ProfileCounterRef ref) {
  auto result = RegionCounterMap.find(ref);
  assert(result != RegionCounterMap.end());
  return result->second;
}
